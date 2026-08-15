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
 *
 * State is tracked per CRTC. It was a single global for as long as every
 * emulated product had exactly one display; JP22 has three, and one shared
 * state sent all three screens' page flips to the first CRTC — see the comment
 * on the kms[] array below.
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

/* One state per CRTC, not one globally.
 *
 * JP22 declares three displays and is the only product that does; with
 * GPU_MAX_OUTPUTS=3 the guest gets three connectors, Qt builds three screens and
 * modesets each one. A single shared state meant the second and third modeset
 * silently reused the first CRTC's ids, and — worse — every page flip from every
 * screen was submitted against that one CRTC and plane. Three flips per frame
 * onto one CRTC exhausts the DRM file's event allocation, which surfaces as
 * atomic commits failing with ENOSPC ("No space left on device") and Qt logging
 * "Could not queue DRM page flip on screen VirtualN".
 *
 * Sized for the largest max_outputs worth supporting; virtio-gpu permits 16 but
 * no Engine product has more than three displays. */
#define MAX_KMS_OUTPUTS 8
static struct kms_state kms[MAX_KMS_OUTPUTS];
static int kms_count;

static struct kms_state *kms_find(uint32_t crtc_id) {
    for (int i = 0; i < kms_count; i++)
        if (kms[i].crtc_id == crtc_id) return &kms[i];
    return NULL;
}

/* True if any already-initialized CRTC has claimed this plane. Planes can be
 * shared between CRTCs in their possible_crtcs mask, so the first match is not
 * automatically free. */
static int plane_is_claimed(uint32_t plane_id) {
    for (int i = 0; i < kms_count; i++)
        if (kms[i].plane_id == plane_id) return 1;
    return 0;
}

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

/* A plane's possible_crtcs is a bitmask of CRTC *indices* — positions in
 * GETRESOURCES' crtc id array — not of CRTC ids. Translate one to the other. */
static int crtc_index_for_id(int fd, uint32_t crtc_id, uint32_t *out_index) {
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (rioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return -1;
    if (res.count_crtcs == 0) return -1;

    uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
    if (!crtc_ids) return -1;
    /* Only the CRTC array is wanted; leaving the other pointers NULL makes the
     * kernel skip them but still refill the counts, so pin the counts we do not
     * want back to zero rather than letting it write through null pointers. */
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    res.count_fbs = res.count_connectors = res.count_encoders = 0;
    int found = -1;
    if (rioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
        for (uint32_t i = 0; i < res.count_crtcs; i++) {
            if (crtc_ids[i] == crtc_id) { *out_index = i; found = 0; break; }
        }
    }
    free(crtc_ids);
    return found;
}

/* Primary plane usable by this specific CRTC, skipping any plane another CRTC
 * has already taken. The previous version returned the first primary plane on
 * the device regardless of CRTC, which is correct only while there is exactly
 * one CRTC. */
static int find_primary_plane(int fd, uint32_t crtc_id, uint32_t *out_plane_id) {
    uint32_t crtc_index;
    if (crtc_index_for_id(fd, crtc_id, &crtc_index) != 0) {
        fprintf(stderr, "=== DRMATOMIC: no index for crtc=%u\n", crtc_id);
        return -1;
    }

    struct drm_mode_get_plane_res pres;
    memset(&pres, 0, sizeof(pres));
    if (rioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0) return -1;
    if (pres.count_planes == 0) return -1;

    uint32_t *plane_ids = calloc(pres.count_planes, sizeof(uint32_t));
    if (!plane_ids) return -1;
    pres.plane_id_ptr = (uint64_t)(uintptr_t)plane_ids;
    if (rioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0) {
        free(plane_ids);
        return -1;
    }

    int found = -1;
    int fallback = -1;
    for (uint32_t i = 0; i < pres.count_planes; i++) {
        if (plane_is_claimed(plane_ids[i])) continue;

        struct drm_mode_get_plane gp;
        memset(&gp, 0, sizeof(gp));
        gp.plane_id = plane_ids[i];
        if (rioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0) continue;
        if (!(gp.possible_crtcs & (1u << crtc_index))) continue;

        if (fallback < 0) fallback = (int)plane_ids[i];

        uint32_t type_prop_id;
        uint64_t type_val;
        if (get_obj_prop_id(fd, plane_ids[i], DRM_MODE_OBJECT_PLANE, "type", &type_prop_id, &type_val) == 0 &&
            type_val == 1 /* DRM_PLANE_TYPE_PRIMARY */) {
            *out_plane_id = plane_ids[i];
            found = 0;
            break;
        }
    }
    /* Fall back to any unclaimed plane this CRTC can drive. Deliberately not the
     * device's first plane as before: on a multi-CRTC device that is very likely
     * to belong to somebody else. */
    if (found != 0 && fallback >= 0) {
        *out_plane_id = (uint32_t)fallback;
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

/* Returns the state for this CRTC, resolving it on first sight. NULL on failure,
 * which leaves the caller to fall back to the legacy ioctl. */
static struct kms_state *ensure_kms_state(int fd, uint32_t crtc_id, uint32_t connector_id,
                                           uint32_t width, uint32_t height) {
    struct kms_state *k = kms_find(crtc_id);
    if (k && k->initialized) {
        /* Re-modeset of a CRTC already resolved. The ids stay valid, but the
         * geometry may not: the plane's SRC_W/SRC_H and CRTC_W/CRTC_H are taken
         * from here, and JP22's three panels need not be the same size. */
        k->width = width;
        k->height = height;
        return k;
    }

    if (!k) {
        if (kms_count >= MAX_KMS_OUTPUTS) {
            fprintf(stderr, "=== DRMATOMIC: more than %d CRTCs in use, ignoring crtc=%u\n",
                    MAX_KMS_OUTPUTS, crtc_id);
            return NULL;
        }
        k = &kms[kms_count];
    }

    memset(k, 0, sizeof(*k));
    k->crtc_id = crtc_id;
    k->connector_id = connector_id;
    k->width = width;
    k->height = height;

    if (ensure_atomic_cap(fd) != 0) return NULL;

    if (find_primary_plane(fd, crtc_id, &k->plane_id) != 0) {
        fprintf(stderr, "=== DRMATOMIC: failed to find a primary plane for crtc=%u\n", crtc_id);
        return NULL;
    }

    int ok = 1;
    ok &= get_obj_prop_id(fd, k->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", &k->crtc_mode_id_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &k->crtc_active_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &k->conn_crtc_id_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &k->plane_fb_id_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &k->plane_crtc_id_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", &k->plane_src_x_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &k->plane_src_y_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", &k->plane_src_w_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", &k->plane_src_h_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &k->plane_crtc_x_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &k->plane_crtc_y_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &k->plane_crtc_w_prop, NULL) == 0;
    ok &= get_obj_prop_id(fd, k->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &k->plane_crtc_h_prop, NULL) == 0;

    if (!ok) {
        fprintf(stderr, "=== DRMATOMIC: failed to resolve property IDs for crtc=%u\n", crtc_id);
        return NULL;
    }
    k->initialized = 1;
    /* Only counted once fully resolved, so a failed attempt does not leave a
     * half-built entry that kms_find() would later hand out. */
    if (k == &kms[kms_count]) kms_count++;
    fprintf(stderr, "=== DRMATOMIC: resolved plane=%u crtc=%u connector=%u (output %d), all props OK\n",
            k->plane_id, k->crtc_id, k->connector_id, kms_count);
    return k;
}

/* modeset!=0: full modeset commit (blob + ACTIVE + CRTC_ID + plane geometry).
 * modeset==0: flip-only commit (just FB_ID on the plane, NONBLOCK|EVENT). */
static int do_atomic_commit(int fd, struct kms_state *k, uint32_t fb_id,
                             const struct drm_mode_modeinfo *mode,
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

    objs[o] = k->plane_id;
    int plane_start = n;
    props[n] = k->plane_fb_id_prop;   vals[n] = fb_id; n++;
    props[n] = k->plane_crtc_id_prop; vals[n] = k->crtc_id; n++;
    if (modeset) {
        props[n] = k->plane_src_x_prop;  vals[n] = 0; n++;
        props[n] = k->plane_src_y_prop;  vals[n] = 0; n++;
        props[n] = k->plane_src_w_prop;  vals[n] = ((uint64_t)k->width) << 16; n++;
        props[n] = k->plane_src_h_prop;  vals[n] = ((uint64_t)k->height) << 16; n++;
        props[n] = k->plane_crtc_x_prop; vals[n] = 0; n++;
        props[n] = k->plane_crtc_y_prop; vals[n] = 0; n++;
        props[n] = k->plane_crtc_w_prop; vals[n] = k->width; n++;
        props[n] = k->plane_crtc_h_prop; vals[n] = k->height; n++;
    }
    counts[o] = n - plane_start;
    o++;

    objs[o] = k->crtc_id;
    int crtc_start = n;
    if (modeset) {
        props[n] = k->crtc_mode_id_prop; vals[n] = mode_blob_id; n++;
        props[n] = k->crtc_active_prop;  vals[n] = 1; n++;
    }
    counts[o] = n - crtc_start;
    o++;

    if (modeset) {
        objs[o] = k->connector_id;
        int conn_start = n;
        props[n] = k->conn_crtc_id_prop; vals[n] = k->crtc_id; n++;
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
            struct kms_state *k = ensure_kms_state(fd, c->crtc_id, connector_id,
                                                   c->mode.hdisplay, c->mode.vdisplay);
            if (k) {
                int r = do_atomic_commit(fd, k, c->fb_id, &c->mode, 1, 0);
                return r < 0 ? -1 : 0;
            }
            fprintf(stderr, "=== DRMATOMIC: kms_state init failed for crtc=%u, falling back to legacy SETCRTC\n",
                    c->crtc_id);
        }
    }

    if (request == DRM_IOCTL_MODE_PAGE_FLIP && arg) {
        struct drm_mode_crtc_page_flip *p = (struct drm_mode_crtc_page_flip *)arg;
        /* Flip on the CRTC the caller named. Previously this used whichever
         * single global state had been resolved first, so with more than one
         * screen every flip landed on the first CRTC — three per frame onto one
         * CRTC, which drains the DRM file's event space and fails ENOSPC. An
         * unknown CRTC falls through to the legacy ioctl rather than guessing. */
        struct kms_state *k = kms_find(p->crtc_id);
        if (k && k->initialized) {
            int r = do_atomic_commit(fd, k, p->fb_id, NULL, 0, p->user_data);
            return r < 0 ? -1 : 0;
        }
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
