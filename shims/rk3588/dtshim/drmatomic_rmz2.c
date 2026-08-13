/* Replaces Qt6 eglfs-kms-gbm's legacy SETCRTC/PAGE_FLIP calls with a
 * hand-rolled DRM_IOCTL_MODE_ATOMIC commit.
 *
 * Both legacy ioctls fail EINVAL under virtio-gpu in this QEMU config even
 * when the submitted modeline is byte-identical to the driver's own
 * GETCONNECTOR-advertised mode (ruled out in drmquirk.c) and even when
 * SETCRTC is skipped outright to test whether the CRTC might already be
 * usably active from boot (inconclusive: PAGE_FLIP still EINVAL). Qt's
 * eglfs-kms-gbm in this build always takes the legacy ioctl path regardless
 * of QT_QPA_EGLFS_KMS_ATOMIC. virtio-gpu's DRM driver is atomic-native in
 * modern kernels, so the theory here is the legacy-to-atomic compat shim in
 * DRM core doesn't cleanly support this call pattern for this device. This
 * shim intercepts SETCRTC/PAGE_FLIP and performs the equivalent state change
 * via a real DRM_IOCTL_MODE_ATOMIC commit instead, bypassing the legacy path
 * entirely.
 *
 * That reimplementation surfaced the actual root cause via drm.debug=0x3ff:
 * virtio-gpu's primary plane rejects Qt's framebuffer outright with "invalid
 * pixel format AR24" (ARGB8888) — the plane only accepts opaque XRGB8888.
 * Same failure regardless of legacy vs. atomic path; this was the real bug
 * both times. ARGB8888 and XRGB8888 share an identical memory layout (the
 * alpha byte is simply unused in XRGB8888), so DRM_IOCTL_MODE_ADDFB2 is
 * intercepted here to rewrite the format fourcc before it reaches the
 * kernel, and the ATOMIC commit logic is kept regardless since it's a more
 * robust primitive than chasing Qt's legacy call shape further.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <drm.h>
#include <drm_mode.h>

typedef int (*ioctl_t)(int, unsigned long, ...);
static ioctl_t real_ioctl = NULL;

#define FOURCC_ARGB8888 0x34325241u /* 'AR24' */
#define FOURCC_XRGB8888 0x34325258u /* 'XR24' */

/* Prefer __ioctl_time64 where it exists, so passthrough keeps exactly the
 * semantics the caller asked for — glibc's time64 entry point converts the
 * timeval-carrying ioctls, and plain ioctl() does not. See the alias at the
 * bottom of this file for why both names matter. */
static void resolve_real_ioctl(void) {
    if (real_ioctl) return;
    real_ioctl = (ioctl_t)dlsym(RTLD_NEXT, "__ioctl_time64");
    if (!real_ioctl) real_ioctl = (ioctl_t)dlsym(RTLD_NEXT, "ioctl");
}

static int rioctl(int fd, unsigned long request, void *arg) {
    resolve_real_ioctl();
    return real_ioctl(fd, request, arg);
}

struct kms_state {
    int initialized;
    uint32_t crtc_id, connector_id, plane_id;
    uint32_t crtc_mode_id_prop, crtc_active_prop;
    uint32_t conn_crtc_id_prop;
    uint32_t plane_fb_id_prop, plane_crtc_id_prop;
    uint32_t plane_src_x_prop, plane_src_y_prop, plane_src_w_prop, plane_src_h_prop;
    uint32_t plane_crtc_x_prop, plane_crtc_y_prop, plane_crtc_w_prop, plane_crtc_h_prop;
    uint32_t width, height;
};
static struct kms_state kms;

/* Two-pass DRM_IOCTL_MODE_OBJ_GETPROPERTIES + per-prop GETPROPERTY name
 * lookup. Returns 0 and fills *out_id (and *out_val if non-NULL) on match. */
static int get_obj_prop_id(int fd, uint32_t obj_id, uint32_t obj_type,
                            const char *name, uint32_t *out_id, uint64_t *out_val) {
    struct drm_mode_obj_get_properties props;
    memset(&props, 0, sizeof(props));
    props.obj_id = obj_id;
    props.obj_type = obj_type;
    if (rioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) < 0) return -1;
    if (props.count_props == 0) return -1;

    uint32_t *prop_ids = calloc(props.count_props, sizeof(uint32_t));
    uint64_t *prop_values = calloc(props.count_props, sizeof(uint64_t));
    props.props_ptr = (uint64_t)(uintptr_t)prop_ids;
    props.prop_values_ptr = (uint64_t)(uintptr_t)prop_values;

    int found = -1;
    if (rioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) == 0) {
        for (uint32_t i = 0; i < props.count_props; i++) {
            struct drm_mode_get_property gp;
            memset(&gp, 0, sizeof(gp));
            gp.prop_id = prop_ids[i];
            if (rioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) == 0 &&
                strcmp(gp.name, name) == 0) {
                *out_id = prop_ids[i];
                if (out_val) *out_val = prop_values[i];
                found = 0;
                break;
            }
        }
    }
    free(prop_ids);
    free(prop_values);
    return found;
}

static int find_primary_plane(int fd, uint32_t *out_plane_id) {
    struct drm_mode_get_plane_res pres;
    memset(&pres, 0, sizeof(pres));
    if (rioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0) return -1;
    if (pres.count_planes == 0) return -1;

    uint32_t *plane_ids = calloc(pres.count_planes, sizeof(uint32_t));
    pres.plane_id_ptr = (uint64_t)(uintptr_t)plane_ids;
    if (rioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0) {
        free(plane_ids);
        return -1;
    }

    int found = -1;
    for (uint32_t i = 0; i < pres.count_planes; i++) {
        uint32_t type_prop_id;
        uint64_t type_val;
        if (get_obj_prop_id(fd, plane_ids[i], DRM_MODE_OBJECT_PLANE, "type", &type_prop_id, &type_val) == 0 &&
            type_val == 1 /* DRM_PLANE_TYPE_PRIMARY */) {
            *out_plane_id = plane_ids[i];
            found = 0;
            break;
        }
    }
    if (found != 0 && pres.count_planes > 0) {
        *out_plane_id = plane_ids[0]; /* fallback: first plane */
        found = 0;
    }
    free(plane_ids);
    return found;
}

/* Qt only ever uses the legacy ioctl path, so it never sets these itself.
 * Without DRM_CLIENT_CAP_ATOMIC, DRM_IOCTL_MODE_ATOMIC is rejected outright;
 * without (the implied) DRM_CLIENT_CAP_UNIVERSAL_PLANES, GETPLANERESOURCES
 * hides primary/cursor planes and find_primary_plane() would come up empty. */
static int ensure_atomic_cap(int fd) {
    struct drm_set_client_cap cap;
    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    cap.value = 1;
    if (rioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        fprintf(stderr, "=== DRMATOMIC: SET_CLIENT_CAP(ATOMIC) failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int ensure_kms_state(int fd, uint32_t crtc_id, uint32_t connector_id,
                             uint32_t width, uint32_t height) {
    if (kms.initialized) return 0;
    memset(&kms, 0, sizeof(kms));
    kms.crtc_id = crtc_id;
    kms.connector_id = connector_id;
    kms.width = width;
    kms.height = height;

    if (ensure_atomic_cap(fd) != 0) return -1;

    if (find_primary_plane(fd, &kms.plane_id) != 0) {
        fprintf(stderr, "=== DRMATOMIC: failed to find a primary plane\n");
        return -1;
    }

    int ok = 1;
    ok &= get_obj_prop_id(fd, kms.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &kms.crtc_mode_id_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &kms.crtc_active_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &kms.conn_crtc_id_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &kms.plane_fb_id_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &kms.plane_crtc_id_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &kms.plane_src_x_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &kms.plane_src_y_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &kms.plane_src_w_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &kms.plane_src_h_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &kms.plane_crtc_x_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &kms.plane_crtc_y_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &kms.plane_crtc_w_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, kms.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &kms.plane_crtc_h_prop, NULL) == 0;

    if (!ok) {
        fprintf(stderr, "=== DRMATOMIC: failed to resolve one or more property IDs\n");
        return -1;
    }
    fprintf(stderr, "=== DRMATOMIC: resolved plane=%u crtc=%u connector=%u, all props OK\n",
            kms.plane_id, kms.crtc_id, kms.connector_id);
    kms.initialized = 1;
    return 0;
}

/* modeset!=0: full modeset commit (blob + ACTIVE + CRTC_ID + plane geometry).
 * modeset==0: flip-only commit (just FB_ID on the plane, NONBLOCK|EVENT). */
static int do_atomic_commit(int fd, uint32_t fb_id, const struct drm_mode_modeinfo *mode,
                             int modeset, uint64_t user_data) {
    uint32_t mode_blob_id = 0;
    if (modeset) {
        struct drm_mode_create_blob blob;
        memset(&blob, 0, sizeof(blob));
        blob.data = (uint64_t)(uintptr_t)mode;
        blob.length = sizeof(*mode);
        if (rioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0) {
            fprintf(stderr, "=== DRMATOMIC: CREATEPROPBLOB failed: %s\n", strerror(errno));
            return -1;
        }
        mode_blob_id = blob.blob_id;
    }

    uint32_t objs[3];
    uint32_t counts[3];
    uint32_t props[16];
    uint64_t vals[16];
    int n = 0, o = 0;

    objs[o] = kms.plane_id;
    int plane_start = n;
    props[n] = kms.plane_fb_id_prop;   vals[n] = fb_id; n++;
    props[n] = kms.plane_crtc_id_prop; vals[n] = kms.crtc_id; n++;
    if (modeset) {
        props[n] = kms.plane_src_x_prop;  vals[n] = 0; n++;
        props[n] = kms.plane_src_y_prop;  vals[n] = 0; n++;
        props[n] = kms.plane_src_w_prop;  vals[n] = ((uint64_t)kms.width) << 16; n++;
        props[n] = kms.plane_src_h_prop;  vals[n] = ((uint64_t)kms.height) << 16; n++;
        props[n] = kms.plane_crtc_x_prop; vals[n] = 0; n++;
        props[n] = kms.plane_crtc_y_prop; vals[n] = 0; n++;
        props[n] = kms.plane_crtc_w_prop; vals[n] = kms.width; n++;
        props[n] = kms.plane_crtc_h_prop; vals[n] = kms.height; n++;
    }
    counts[o] = n - plane_start;
    o++;

    objs[o] = kms.crtc_id;
    int crtc_start = n;
    if (modeset) {
        props[n] = kms.crtc_mode_id_prop; vals[n] = mode_blob_id; n++;
        props[n] = kms.crtc_active_prop;  vals[n] = 1; n++;
    }
    counts[o] = n - crtc_start;
    o++;

    if (modeset) {
        objs[o] = kms.connector_id;
        int conn_start = n;
        props[n] = kms.conn_crtc_id_prop; vals[n] = kms.crtc_id; n++;
        counts[o] = n - conn_start;
        o++;
    }

    struct drm_mode_atomic atomic;
    memset(&atomic, 0, sizeof(atomic));
    atomic.flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET
                           : (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT);
    atomic.count_objs = o;
    atomic.objs_ptr = (uint64_t)(uintptr_t)objs;
    atomic.count_props_ptr = (uint64_t)(uintptr_t)counts;
    atomic.props_ptr = (uint64_t)(uintptr_t)props;
    atomic.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    atomic.user_data = user_data;

    int ret = rioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic);
    /* A FLIP commit happens once per rendered frame, so logging it
     * unconditionally puts a synchronous write to stderr — which under
     * engine.service means a journald round-trip — directly in the render
     * path, at frame rate. That is expensive enough to be felt as general
     * slowness (and to starve the audio thread into dropouts). Modesets are
     * rare and diagnostically valuable, so those still log by default;
     * per-frame flips only with DRMATOMIC_DEBUG set. */
    if (modeset || ret < 0 || getenv("DRMATOMIC_DEBUG"))
        fprintf(stderr,
                "=== DRMATOMIC: %s commit (objs=%d props=%d flags=0x%x) -> ret=%d errno=%d (%s)\n",
                modeset ? "MODESET" : "FLIP", o, n, atomic.flags, ret,
                ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "ok");
    return ret;
}

int ioctl(int fd, unsigned long request, ...) {
    resolve_real_ioctl();
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (request == DRM_IOCTL_MODE_ADDFB2 && arg) {
        struct drm_mode_fb_cmd2 *f = (struct drm_mode_fb_cmd2 *)arg;
        if (f->pixel_format == FOURCC_ARGB8888) {
            fprintf(stderr, "=== DRMATOMIC: ADDFB2 rewriting pixel_format ARGB8888 -> XRGB8888 (virtio-gpu primary plane rejects alpha formats)\n");
            f->pixel_format = FOURCC_XRGB8888;
        }
    }

    if (request == DRM_IOCTL_MODE_SETCRTC && arg) {
        struct drm_mode_crtc *c = (struct drm_mode_crtc *)arg;
        if (c->mode_valid) {
            uint32_t connector_id = 0;
            if (c->count_connectors > 0 && c->set_connectors_ptr) {
                connector_id = ((uint32_t *)(uintptr_t)c->set_connectors_ptr)[0];
            }
            if (ensure_kms_state(fd, c->crtc_id, connector_id, c->mode.hdisplay, c->mode.vdisplay) == 0) {
                int r = do_atomic_commit(fd, c->fb_id, &c->mode, 1, 0);
                return r < 0 ? -1 : 0;
            }
            fprintf(stderr, "=== DRMATOMIC: kms_state init failed, falling back to legacy SETCRTC\n");
        }
    }

    if (request == DRM_IOCTL_MODE_PAGE_FLIP && arg && kms.initialized) {
        struct drm_mode_crtc_page_flip *p = (struct drm_mode_crtc_page_flip *)arg;
        int r = do_atomic_commit(fd, p->fb_id, NULL, 0, p->user_data);
        return r < 0 ? -1 : 0;
    }

    int ret = real_ioctl(fd, request, arg);

    if (request == DRM_IOCTL_MODE_SETCRTC || request == DRM_IOCTL_MODE_PAGE_FLIP) {
        fprintf(stderr, "=== DRMATOMIC: legacy %s passthrough result: ret=%d errno=%d (%s)\n",
                request == DRM_IOCTL_MODE_SETCRTC ? "SETCRTC" : "PAGE_FLIP",
                ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "ok");
    }

    return ret;
}

/* The name 32-bit callers actually import. glibc >= 2.34 on a 32-bit port built
 * with 64-bit time_t redirects ioctl() to __ioctl_time64() in <sys/ioctl.h>, so
 * that is the symbol that lands in the caller's relocations: the armv7 Engine OS
 * rootfs's libdrm has an undefined __ioctl_time64@GLIBC_2.34 and no reference to
 * plain `ioctl` at all. Exporting only `ioctl` therefore interposes nothing there
 * — silently, since the shim loads fine and simply never runs, which presents as
 * Qt's "Could not queue DRM page flip ... (Invalid argument)" and a black screen,
 * exactly the failure this file exists to fix.
 *
 * The arm64 rootfs's libdrm imports ioctl@GLIBC_2.17, which is why this never
 * came up there. aarch64 glibc has no __ioctl_time64 and nothing looks the name
 * up, so defining it is inert on that side. */
int __ioctl_time64(int fd, unsigned long request, ...) __attribute__((alias("ioctl")));
