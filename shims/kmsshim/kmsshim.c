/*
 * kmsshim.c — a fake KMS device for apps that drive the display themselves,
 * so they can run in a window under a Wayland compositor instead of owning a
 * screen. Preloaded into the app; kmspresent, on the host, shows its frames.
 *
 * MPC (and the rest of inMusic's JUCE apps) open /dev/dri/card0, become DRM
 * master and page-flip their own dumb buffers with atomic commits. A
 * compositor already holds the display, so here /dev/dri/card* opens a pipe
 * instead and every DRM ioctl on it is answered from a model of one simple
 * device: one connector with one mode, one encoder, one CRTC and one primary
 * plane taking XRGB/ARGB8888. What MPC 3.10.0 Gen 2 uses, from strace on a
 * virtio-gpu guest: GET_CAP, SET_CLIENT_CAP(ATOMIC), SET_MASTER, GETRESOURCES,
 * GETPLANERESOURCES, GETPLANE, OBJ_GETPROPERTIES, GETPROPERTY, GETCONNECTOR,
 * GETENCODER, CREATE_DUMB, MAP_DUMB, ADDFB2, CREATEPROPBLOB/DESTROYPROPBLOB
 * (a FB_DAMAGE_CLIPS rect each frame) and ATOMIC with PAGE_FLIP_EVENT, whose
 * events it reads back with drmHandleEvent.
 *
 * Dumb buffers are memfds, mapped by the app through mmap() on the fake fd at
 * the offset MAP_DUMB gave. kmspresent gets the same memfds over a unix
 * socket (SCM_RIGHTS) and hands them to the compositor as wl_shm buffers, so
 * no frame is copied. Flip events are paced by a 60 Hz clock of our own, not
 * by the compositor: a window that is hidden gets no frame callbacks, and the
 * app must not stall because of it. The app keeps running with no presenter
 * at all; frames then go nowhere.
 *
 * Environment:
 *   KMSSHIM_SOCKET  where kmspresent listens. Default /run/kmsshim.sock
 *   KMSSHIM_MODE    the one mode offered, WxH. Default 1280x800
 *   KMSSHIM_LOG     a file to log to; unhandled ioctls are always logged there
 *   KMSSHIM_SEAT    the udev seat kmspresent's touchscreen is on. When set, the
 *                   app's libinput sees that seat's devices as seat0, and no others
 *
 * Build: gcc -shared -fPIC -O2 -I/usr/include/libdrm -o kmsshim.so kmsshim.c -ldl -lpthread
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include "kmsproto.h"

/* Object ids, as a real driver would hand out: any distinct numbers do. */
enum { PLANE_ID = 31, CRTC_ID_OBJ = 32, CONNECTOR_ID = 33, ENCODER_ID = 34 };

/* Properties. Ids are ours; names and flags are what the kernel uses, since
 * the app finds them by name. */
enum {
    P_EDID = 1, P_DPMS, P_LINK_STATUS, P_NON_DESKTOP, P_CONN_CRTC_ID,
    P_ACTIVE, P_MODE_ID, P_VRR_ENABLED, P_OUT_FENCE_PTR,
    P_TYPE, P_SRC_X, P_SRC_Y, P_SRC_W, P_SRC_H, P_CRTC_X, P_CRTC_Y, P_CRTC_W,
    P_CRTC_H, P_FB_ID, P_IN_FENCE_FD, P_PLANE_CRTC_ID, P_FB_DAMAGE_CLIPS,
    P_COUNT
};
struct penum { uint64_t value; const char *name; };
struct prop {
    const char *name;
    uint32_t flags;
    uint64_t v[2];           /* range min/max, or object type */
    int nv;
    const struct penum *e;   /* enum entries */
    int ne;
};
static const struct penum dpms_e[] = {{0, "On"}, {1, "Standby"}, {2, "Suspend"}, {3, "Off"}};
static const struct penum link_e[] = {{0, "Good"}, {1, "Bad"}};
static const struct penum type_e[] = {{0, "Overlay"}, {1, "Primary"}, {2, "Cursor"}};
#define A DRM_MODE_PROP_ATOMIC
static const struct prop props[P_COUNT] = {
    [P_EDID]          = {"EDID", DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE, {0}, 0, 0, 0},
    [P_DPMS]          = {"DPMS", DRM_MODE_PROP_ENUM, {0}, 0, dpms_e, 4},
    [P_LINK_STATUS]   = {"link-status", DRM_MODE_PROP_ENUM, {0}, 0, link_e, 2},
    [P_NON_DESKTOP]   = {"non-desktop", DRM_MODE_PROP_RANGE | DRM_MODE_PROP_IMMUTABLE, {0, 1}, 2, 0, 0},
    [P_CONN_CRTC_ID]  = {"CRTC_ID", A | DRM_MODE_PROP_OBJECT, {DRM_MODE_OBJECT_CRTC}, 1, 0, 0},
    [P_ACTIVE]        = {"ACTIVE", A | DRM_MODE_PROP_RANGE, {0, 1}, 2, 0, 0},
    [P_MODE_ID]       = {"MODE_ID", A | DRM_MODE_PROP_BLOB, {0}, 0, 0, 0},
    [P_VRR_ENABLED]   = {"VRR_ENABLED", DRM_MODE_PROP_RANGE, {0, 1}, 2, 0, 0},
    [P_OUT_FENCE_PTR] = {"OUT_FENCE_PTR", A | DRM_MODE_PROP_RANGE, {0, UINT64_MAX}, 2, 0, 0},
    [P_TYPE]          = {"type", DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE, {0}, 0, type_e, 3},
    [P_SRC_X]         = {"SRC_X", A | DRM_MODE_PROP_RANGE, {0, UINT32_MAX}, 2, 0, 0},
    [P_SRC_Y]         = {"SRC_Y", A | DRM_MODE_PROP_RANGE, {0, UINT32_MAX}, 2, 0, 0},
    [P_SRC_W]         = {"SRC_W", A | DRM_MODE_PROP_RANGE, {0, UINT32_MAX}, 2, 0, 0},
    [P_SRC_H]         = {"SRC_H", A | DRM_MODE_PROP_RANGE, {0, UINT32_MAX}, 2, 0, 0},
    [P_CRTC_X]        = {"CRTC_X", A | DRM_MODE_PROP_SIGNED_RANGE, {(uint64_t)INT32_MIN, INT32_MAX}, 2, 0, 0},
    [P_CRTC_Y]        = {"CRTC_Y", A | DRM_MODE_PROP_SIGNED_RANGE, {(uint64_t)INT32_MIN, INT32_MAX}, 2, 0, 0},
    [P_CRTC_W]        = {"CRTC_W", A | DRM_MODE_PROP_RANGE, {0, INT32_MAX}, 2, 0, 0},
    [P_CRTC_H]        = {"CRTC_H", A | DRM_MODE_PROP_RANGE, {0, INT32_MAX}, 2, 0, 0},
    [P_FB_ID]         = {"FB_ID", A | DRM_MODE_PROP_OBJECT, {DRM_MODE_OBJECT_FB}, 1, 0, 0},
    [P_IN_FENCE_FD]   = {"IN_FENCE_FD", A | DRM_MODE_PROP_SIGNED_RANGE, {(uint64_t)-1, INT32_MAX}, 2, 0, 0},
    [P_PLANE_CRTC_ID] = {"CRTC_ID", A | DRM_MODE_PROP_OBJECT, {DRM_MODE_OBJECT_CRTC}, 1, 0, 0},
    [P_FB_DAMAGE_CLIPS] = {"FB_DAMAGE_CLIPS", A | DRM_MODE_PROP_BLOB, {0}, 0, 0, 0},
};
#undef A

static const int conn_props[] = {P_EDID, P_DPMS, P_LINK_STATUS, P_NON_DESKTOP, P_CONN_CRTC_ID};
static const int crtc_props[] = {P_ACTIVE, P_MODE_ID, P_VRR_ENABLED, P_OUT_FENCE_PTR};
static const int plane_props[] = {P_TYPE, P_SRC_X, P_SRC_Y, P_SRC_W, P_SRC_H, P_CRTC_X,
                                  P_CRTC_Y, P_CRTC_W, P_CRTC_H, P_FB_ID, P_IN_FENCE_FD,
                                  P_PLANE_CRTC_ID, P_FB_DAMAGE_CLIPS};
static uint64_t pval[P_COUNT];  /* current values; one object of each kind */

#define MAX_BUFS 256
#define MAX_FBS 256
#define MAX_BLOBS 64
struct dumb { int used; int memfd; uint32_t w, h, pitch; uint64_t size; int sent; };
struct fb { int used; uint32_t handle, w, h, pitch, offset, format; int sent; };
struct blob { int used; uint32_t len; void *data; };
static struct dumb bufs[MAX_BUFS];   /* by handle */
static struct fb fbs[MAX_FBS];       /* by fb id - FB_BASE */
static struct blob blobs[MAX_BLOBS]; /* by blob id - BLOB_BASE */
#define FB_BASE 100
#define BLOB_BASE 1000
#define MAP_SHIFT 32  /* MAP_DUMB offset = handle << MAP_SHIFT */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flip_cv = PTHREAD_COND_INITIALIZER;
static int fake_rd = -1, fake_wr = -1;   /* the app's fd and its event pipe */
static int sock = -1;                    /* to kmspresent */
static time_t last_connect;
static uint32_t mode_w = 1280, mode_h = 800;
static struct drm_mode_modeinfo mode;
static int flip_pending; static uint64_t flip_user_data;
static uint32_t vbl_seq;
static FILE *logf;

static void lg(const char *fmt, ...) {
    if (!logf) return;
    va_list ap; va_start(ap, fmt);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(logf, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    vfprintf(logf, fmt, ap); fputc('\n', logf); fflush(logf);
    va_end(ap);
}

static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_close)(int);

static void make_mode(void) {
    /* CVT reduced blanking timings; only the visible size matters here. */
    memset(&mode, 0, sizeof mode);
    mode.hdisplay = mode_w; mode.hsync_start = mode_w + 48; mode.hsync_end = mode_w + 80;
    mode.htotal = mode_w + 160;
    mode.vdisplay = mode_h; mode.vsync_start = mode_h + 3; mode.vsync_end = mode_h + 9;
    mode.vtotal = mode_h + 23;
    mode.vrefresh = 60;
    mode.clock = (uint32_t)((uint64_t)mode.htotal * mode.vtotal * 60 / 1000);
    mode.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode.type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    snprintf(mode.name, sizeof mode.name, "%ux%u", mode_w, mode_h);
}

static void *vblank_thread(void *arg);

__attribute__((constructor)) static void init(void) {
    real_open = dlsym(RTLD_NEXT, "open");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_mmap = dlsym(RTLD_NEXT, "mmap");
    real_close = dlsym(RTLD_NEXT, "close");
    const char *l = getenv("KMSSHIM_LOG");
    if (l && *l) logf = fopen(l, "ae");
    const char *m = getenv("KMSSHIM_MODE");
    if (m && sscanf(m, "%ux%u", &mode_w, &mode_h) != 2) { mode_w = 1280; mode_h = 800; }
    make_mode();
    pval[P_DPMS] = 0; pval[P_CONN_CRTC_ID] = CRTC_ID_OBJ;
    pval[P_ACTIVE] = 1; pval[P_TYPE] = 1; pval[P_PLANE_CRTC_ID] = CRTC_ID_OBJ;
    pval[P_IN_FENCE_FD] = (uint64_t)-1;
}

/* ---- the presenter connection ------------------------------------------ */

static int send_msg(const struct kmsmsg *msg, int fd) {
    if (sock < 0) return -1;
    struct iovec iov = {(void *)msg, sizeof *msg};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = {0};
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    if (fd >= 0) {
        memset(cbuf, 0, sizeof cbuf);
        mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(sock, &mh, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
        if (errno == EAGAIN) return 0;   /* a slow presenter drops a frame */
        lg("presenter gone (%s)", strerror(errno));
        real_close(sock); sock = -1;
        return -1;
    }
    return 0;
}

static int send_buf(uint32_t h) {
    struct kmsmsg m = {.type = KMS_BUF, .id = h, .size = bufs[h].size};
    if (send_msg(&m, bufs[h].memfd) < 0) return -1;
    bufs[h].sent = 1; return 0;
}

static int send_fb(uint32_t i) {
    struct fb *f = &fbs[i];
    if (!bufs[f->handle].sent && send_buf(f->handle) < 0) return -1;
    struct kmsmsg m = {.type = KMS_FB, .id = i + FB_BASE, .buf = f->handle, .offset = f->offset,
                       .w = f->w, .h = f->h, .pitch = f->pitch, .format = f->format};
    if (send_msg(&m, -1) < 0) return -1;
    f->sent = 1; return 0;
}

/* Lazily, from the commit path, and at most once a second, so that a
 * presenter started (or restarted) after the app is picked up. Everything the
 * old one had is sent again. */
static void try_connect(void) {
    if (sock >= 0) return;
    time_t now = time(NULL);
    if (now == last_connect) return;
    last_connect = now;
    const char *path = getenv("KMSSHIM_SOCKET");
    if (!path || !*path) path = "/run/kmsshim.sock";
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { real_close(s); return; }
    int sz = 1 << 20; setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    sock = s;
    lg("connected to presenter at %s", path);
    for (int i = 0; i < MAX_BUFS; i++) bufs[i].sent = 0;
    for (int i = 0; i < MAX_FBS; i++) fbs[i].sent = 0;
    struct kmsmsg m = {.type = KMS_MODE, .w = mode_w, .h = mode_h};
    send_msg(&m, -1);
}

static void present(uint32_t fb_id, const struct drm_mode_rect *clips, int nclips) {
    try_connect();
    if (sock < 0) return;
    uint32_t i = fb_id - FB_BASE;
    if (i >= MAX_FBS || !fbs[i].used) return;
    if (!fbs[i].sent && send_fb(i) < 0) return;
    struct kmsmsg m = {.type = KMS_SHOW, .id = fb_id, .x1 = 0, .y1 = 0, .x2 = fbs[i].w, .y2 = fbs[i].h};
    if (nclips > 0) {
        /* One bounding box is plenty: the compositor only uses it as a hint. */
        int32_t x1 = INT32_MAX, y1 = INT32_MAX, x2 = INT32_MIN, y2 = INT32_MIN;
        for (int c = 0; c < nclips; c++) {
            if (clips[c].x1 < x1) x1 = clips[c].x1;
            if (clips[c].y1 < y1) y1 = clips[c].y1;
            if (clips[c].x2 > x2) x2 = clips[c].x2;
            if (clips[c].y2 > y2) y2 = clips[c].y2;
        }
        m.x1 = x1; m.y1 = y1; m.x2 = x2; m.y2 = y2;
    }
    send_msg(&m, -1);
}

/* ---- vblank -------------------------------------------------------------- */

static void *vblank_thread(void *arg) {
    (void)arg;
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        pthread_mutex_lock(&lk);
        while (!flip_pending || fake_wr < 0) pthread_cond_wait(&flip_cv, &lk);
        pthread_mutex_unlock(&lk);

        /* The next tick of a 60 Hz clock, or now if we have fallen behind. */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        next.tv_nsec += 16666667;
        if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
        if (next.tv_sec < now.tv_sec || (next.tv_sec == now.tv_sec && next.tv_nsec < now.tv_nsec))
            next = now;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        pthread_mutex_lock(&lk);
        if (flip_pending && fake_wr >= 0) {
            struct drm_event_vblank ev = {0};
            ev.base.type = DRM_EVENT_FLIP_COMPLETE;
            ev.base.length = sizeof ev;
            ev.user_data = flip_user_data;
            ev.tv_sec = (uint32_t)next.tv_sec;
            ev.tv_usec = (uint32_t)(next.tv_nsec / 1000);
            ev.sequence = ++vbl_seq;
            ev.crtc_id = CRTC_ID_OBJ;
            if (write(fake_wr, &ev, sizeof ev) != sizeof ev) lg("event write failed");
            flip_pending = 0;
        }
        pthread_mutex_unlock(&lk);
    }
    return NULL;
}

/* ---- ioctls -------------------------------------------------------------- */

#define U64P(x) ((void *)(uintptr_t)(x))

static void put_u32s(uint64_t ptr, uint32_t room, const uint32_t *v, uint32_t n) {
    if (ptr && room >= n) memcpy(U64P(ptr), v, n * sizeof *v);
}

static int obj_props(uint32_t id, const int **list) {
    switch (id) {
    case CONNECTOR_ID: *list = conn_props; return sizeof conn_props / sizeof *conn_props;
    case CRTC_ID_OBJ: *list = crtc_props; return sizeof crtc_props / sizeof *crtc_props;
    case PLANE_ID: *list = plane_props; return sizeof plane_props / sizeof *plane_props;
    }
    return -1;
}

static int set_prop(uint32_t obj, uint32_t pid, uint64_t v) {
    const int *list; int n = obj_props(obj, &list);
    for (int i = 0; i < n; i++)
        if (list[i] == (int)pid) { pval[pid] = v; return 0; }
    lg("atomic: object %u has no property %u", obj, pid);
    return -EINVAL;
}

static int do_atomic(struct drm_mode_atomic *a) {
    uint32_t *objs = U64P(a->objs_ptr), *cnt = U64P(a->count_props_ptr);
    uint32_t *pids = U64P(a->props_ptr);
    uint64_t *vals = U64P(a->prop_values_ptr);
    uint64_t saved[P_COUNT]; memcpy(saved, pval, sizeof pval);
    uint32_t k = 0;
    for (uint32_t o = 0; o < a->count_objs; o++)
        for (uint32_t p = 0; p < cnt[o]; p++, k++) {
            int r = set_prop(objs[o], pids[k], vals[k]);
            if (r) { memcpy(pval, saved, sizeof pval); return r; }
        }
    if (a->flags & DRM_MODE_ATOMIC_TEST_ONLY) { memcpy(pval, saved, sizeof pval); return 0; }
    if ((a->flags & DRM_MODE_PAGE_FLIP_EVENT) && flip_pending) {
        memcpy(pval, saved, sizeof pval); return -EBUSY;
    }
    const struct drm_mode_rect *clips = NULL; int nclips = 0;
    uint32_t b = (uint32_t)pval[P_FB_DAMAGE_CLIPS] - BLOB_BASE;
    if (pval[P_FB_DAMAGE_CLIPS] && b < MAX_BLOBS && blobs[b].used) {
        clips = blobs[b].data; nclips = blobs[b].len / sizeof *clips;
    }
    if (pval[P_ACTIVE] && pval[P_FB_ID]) present((uint32_t)pval[P_FB_ID], clips, nclips);
    /* FB_DAMAGE_CLIPS applies to the commit that set it, not the ones after. */
    pval[P_FB_DAMAGE_CLIPS] = 0;
    if (a->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        flip_pending = 1; flip_user_data = a->user_data;
        pthread_cond_signal(&flip_cv);
    }
    return 0;
}

static int drm_ioctl(unsigned long req, void *arg) {
    switch (_IOC_NR(req)) {
    case _IOC_NR(DRM_IOCTL_VERSION): {
        struct drm_version *v = arg;
        static const char name[] = "kmsshim", date[] = "20261112", desc[] = "QEngine fake KMS";
        v->version_major = 1; v->version_minor = 0; v->version_patchlevel = 0;
#define S(f, s) do { if (v->f && v->f##_len) memcpy(v->f, s, v->f##_len < sizeof s ? v->f##_len : sizeof s); v->f##_len = sizeof s - 1; } while (0)
        S(name, name); S(date, date); S(desc, desc);
#undef S
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_GET_CAP): {
        struct drm_get_cap *c = arg;
        switch (c->capability) {
        case DRM_CAP_DUMB_BUFFER: case DRM_CAP_TIMESTAMP_MONOTONIC:
        case DRM_CAP_CRTC_IN_VBLANK_EVENT: case DRM_CAP_VBLANK_HIGH_CRTC: c->value = 1; break;
        case DRM_CAP_DUMB_PREFERRED_DEPTH: c->value = 24; break;
        case DRM_CAP_CURSOR_WIDTH: case DRM_CAP_CURSOR_HEIGHT: c->value = 64; break;
        default: c->value = 0;
        }
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_SET_CLIENT_CAP):
    case _IOC_NR(DRM_IOCTL_SET_MASTER):
    case _IOC_NR(DRM_IOCTL_DROP_MASTER):
    case _IOC_NR(DRM_IOCTL_GET_MAGIC):
    case _IOC_NR(DRM_IOCTL_AUTH_MAGIC):
    case _IOC_NR(DRM_IOCTL_MODE_DIRTYFB):
    /* MPC Sample 1.3.0 hides a hardware cursor it was never going to draw. The
     * window's cursor is the compositor's, so there is none to hide. */
    case _IOC_NR(DRM_IOCTL_MODE_CURSOR):
    case _IOC_NR(DRM_IOCTL_MODE_CURSOR2):
        return 0;
    case _IOC_NR(DRM_IOCTL_MODE_GETRESOURCES): {
        struct drm_mode_card_res *r = arg;
        uint32_t c = CRTC_ID_OBJ, n = CONNECTOR_ID, e = ENCODER_ID;
        put_u32s(r->crtc_id_ptr, r->count_crtcs, &c, 1);
        put_u32s(r->connector_id_ptr, r->count_connectors, &n, 1);
        put_u32s(r->encoder_id_ptr, r->count_encoders, &e, 1);
        r->count_fbs = 0; r->count_crtcs = 1; r->count_connectors = 1; r->count_encoders = 1;
        r->min_width = 1; r->max_width = 8192; r->min_height = 1; r->max_height = 8192;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_GETPLANERESOURCES): {
        struct drm_mode_get_plane_res *r = arg;
        uint32_t p = PLANE_ID;
        put_u32s(r->plane_id_ptr, r->count_planes, &p, 1);
        r->count_planes = 1;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_GETPLANE): {
        struct drm_mode_get_plane *p = arg;
        if (p->plane_id != PLANE_ID) return -ENOENT;
        static const uint32_t fmts[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
        put_u32s(p->format_type_ptr, p->count_format_types, fmts, 2);
        p->count_format_types = 2;
        p->crtc_id = CRTC_ID_OBJ; p->fb_id = (uint32_t)pval[P_FB_ID];
        p->possible_crtcs = 1; p->gamma_size = 0;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_GETCRTC): {
        struct drm_mode_crtc *c = arg;
        if (c->crtc_id != CRTC_ID_OBJ) return -ENOENT;
        c->fb_id = (uint32_t)pval[P_FB_ID]; c->x = c->y = 0; c->gamma_size = 0;
        c->mode_valid = 1; c->mode = mode;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_GETENCODER): {
        struct drm_mode_get_encoder *e = arg;
        if (e->encoder_id != ENCODER_ID) return -ENOENT;
        e->encoder_type = DRM_MODE_ENCODER_VIRTUAL; e->crtc_id = CRTC_ID_OBJ;
        e->possible_crtcs = 1; e->possible_clones = 0;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_GETCONNECTOR): {
        struct drm_mode_get_connector *c = arg;
        if (c->connector_id != CONNECTOR_ID) return -ENOENT;
        uint32_t e = ENCODER_ID;
        put_u32s(c->encoders_ptr, c->count_encoders, &e, 1);
        if (c->modes_ptr && c->count_modes >= 1) memcpy(U64P(c->modes_ptr), &mode, sizeof mode);
        uint32_t n = sizeof conn_props / sizeof *conn_props;
        if (c->props_ptr && c->prop_values_ptr && c->count_props >= n)
            for (uint32_t i = 0; i < n; i++) {
                ((uint32_t *)U64P(c->props_ptr))[i] = conn_props[i];
                ((uint64_t *)U64P(c->prop_values_ptr))[i] = pval[conn_props[i]];
            }
        c->count_encoders = 1; c->count_modes = 1; c->count_props = n;
        c->encoder_id = ENCODER_ID;
        c->connector_type = DRM_MODE_CONNECTOR_VIRTUAL; c->connector_type_id = 1;
        c->connection = 1;  /* connected */
        c->mm_width = mode_w * 254 / 1000 / 2; c->mm_height = mode_h * 254 / 1000 / 2;
        c->subpixel = 1;  /* DRM_MODE_SUBPIXEL_UNKNOWN, a libdrm name, not the uapi one */
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_OBJ_GETPROPERTIES): {
        struct drm_mode_obj_get_properties *o = arg;
        const int *list; int n = obj_props(o->obj_id, &list);
        if (n < 0) return -ENOENT;
        if (o->props_ptr && o->prop_values_ptr && o->count_props >= (uint32_t)n)
            for (int i = 0; i < n; i++) {
                ((uint32_t *)U64P(o->props_ptr))[i] = list[i];
                ((uint64_t *)U64P(o->prop_values_ptr))[i] = pval[list[i]];
            }
        o->count_props = n;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_GETPROPERTY): {
        struct drm_mode_get_property *g = arg;
        if (g->prop_id == 0 || g->prop_id >= P_COUNT) return -ENOENT;
        const struct prop *p = &props[g->prop_id];
        snprintf(g->name, sizeof g->name, "%s", p->name);
        g->flags = p->flags;
        if (p->ne) {
            if (g->values_ptr && g->count_values >= (uint32_t)p->ne)
                for (int i = 0; i < p->ne; i++) ((uint64_t *)U64P(g->values_ptr))[i] = p->e[i].value;
            if (g->enum_blob_ptr && g->count_enum_blobs >= (uint32_t)p->ne)
                for (int i = 0; i < p->ne; i++) {
                    struct drm_mode_property_enum *pe = (struct drm_mode_property_enum *)U64P(g->enum_blob_ptr) + i;
                    pe->value = p->e[i].value;
                    snprintf(pe->name, sizeof pe->name, "%s", p->e[i].name);
                }
            g->count_values = g->count_enum_blobs = p->ne;
        } else {
            if (g->values_ptr && g->count_values >= (uint32_t)p->nv)
                memcpy(U64P(g->values_ptr), p->v, p->nv * sizeof(uint64_t));
            g->count_values = p->nv; g->count_enum_blobs = 0;
        }
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_CREATEPROPBLOB): {
        struct drm_mode_create_blob *c = arg;
        for (int i = 0; i < MAX_BLOBS; i++)
            if (!blobs[i].used) {
                blobs[i].data = malloc(c->length ? c->length : 1);
                if (!blobs[i].data) return -ENOMEM;
                memcpy(blobs[i].data, U64P(c->data), c->length);
                blobs[i].len = c->length; blobs[i].used = 1;
                c->blob_id = BLOB_BASE + i;
                return 0;
            }
        return -ENOSPC;
    }
    case _IOC_NR(DRM_IOCTL_MODE_DESTROYPROPBLOB): {
        struct drm_mode_destroy_blob *d = arg;
        uint32_t i = d->blob_id - BLOB_BASE;
        if (i >= MAX_BLOBS || !blobs[i].used) return -ENOENT;
        free(blobs[i].data); blobs[i].used = 0;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_GETPROPBLOB): {
        struct drm_mode_get_blob *g = arg;
        uint32_t i = g->blob_id - BLOB_BASE;
        if (i >= MAX_BLOBS || !blobs[i].used) return -ENOENT;
        if (g->data && g->length >= blobs[i].len) memcpy(U64P(g->data), blobs[i].data, blobs[i].len);
        g->length = blobs[i].len;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_CREATE_DUMB): {
        struct drm_mode_create_dumb *c = arg;
        for (uint32_t h = 1; h < MAX_BUFS; h++)
            if (!bufs[h].used) {
                uint32_t pitch = (c->width * ((c->bpp + 7) / 8) + 63) & ~63u;
                uint64_t size = ((uint64_t)pitch * c->height + 4095) & ~4095ull;
                int fd = memfd_create("kmsshim-dumb", MFD_CLOEXEC);
                if (fd < 0) return -errno;
                if (ftruncate(fd, size) < 0) { real_close(fd); return -ENOMEM; }
                bufs[h] = (struct dumb){1, fd, c->width, c->height, pitch, size, 0};
                c->handle = h; c->pitch = pitch; c->size = size;
                return 0;
            }
        return -ENOSPC;
    }
    case _IOC_NR(DRM_IOCTL_MODE_MAP_DUMB): {
        struct drm_mode_map_dumb *m = arg;
        if (m->handle >= MAX_BUFS || !bufs[m->handle].used) return -ENOENT;
        m->offset = (uint64_t)m->handle << MAP_SHIFT;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_DESTROY_DUMB):
    case _IOC_NR(DRM_IOCTL_GEM_CLOSE): {
        uint32_t h = *(uint32_t *)arg;
        if (h >= MAX_BUFS || !bufs[h].used) return -ENOENT;
        if (bufs[h].sent) { struct kmsmsg m = {.type = KMS_FREEBUF, .id = h}; send_msg(&m, -1); }
        real_close(bufs[h].memfd); bufs[h].used = 0;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_ADDFB2):
    case _IOC_NR(DRM_IOCTL_MODE_ADDFB): {
        uint32_t handle, w, h, pitch, offset = 0, format, *idp;
        if (_IOC_NR(req) == _IOC_NR(DRM_IOCTL_MODE_ADDFB2)) {
            struct drm_mode_fb_cmd2 *f = arg;
            handle = f->handles[0]; w = f->width; h = f->height; pitch = f->pitches[0];
            offset = f->offsets[0]; format = f->pixel_format; idp = &f->fb_id;
        } else {
            struct drm_mode_fb_cmd *f = arg;
            handle = f->handle; w = f->width; h = f->height; pitch = f->pitch;
            format = f->depth == 32 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888; idp = &f->fb_id;
        }
        if (handle >= MAX_BUFS || !bufs[handle].used) return -ENOENT;
        if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
            lg("ADDFB: unsupported format %.4s", (char *)&format);
            return -EINVAL;
        }
        for (uint32_t i = 0; i < MAX_FBS; i++)
            if (!fbs[i].used) {
                fbs[i] = (struct fb){1, handle, w, h, pitch, offset, format, 0};
                *idp = i + FB_BASE;
                return 0;
            }
        return -ENOSPC;
    }
    case _IOC_NR(DRM_IOCTL_MODE_RMFB):
#ifdef DRM_IOCTL_MODE_CLOSEFB
    case _IOC_NR(DRM_IOCTL_MODE_CLOSEFB):
#endif
    {
        uint32_t i = *(uint32_t *)arg - FB_BASE;
        if (i >= MAX_FBS || !fbs[i].used) return -ENOENT;
        if (fbs[i].sent) { struct kmsmsg m = {.type = KMS_RMFB, .id = i + FB_BASE}; send_msg(&m, -1); }
        fbs[i].used = 0;
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_ATOMIC):
        return do_atomic(arg);
    case _IOC_NR(DRM_IOCTL_MODE_SETCRTC): {
        struct drm_mode_crtc *c = arg;
        pval[P_FB_ID] = c->fb_id; pval[P_ACTIVE] = c->fb_id != 0;
        if (c->fb_id) present(c->fb_id, NULL, 0);
        return 0;
    }
    case _IOC_NR(DRM_IOCTL_MODE_PAGE_FLIP): {
        struct drm_mode_crtc_page_flip *f = arg;
        if (flip_pending) return -EBUSY;
        pval[P_FB_ID] = f->fb_id;
        present(f->fb_id, NULL, 0);
        if (f->flags & DRM_MODE_PAGE_FLIP_EVENT) {
            flip_pending = 1; flip_user_data = f->user_data;
            pthread_cond_signal(&flip_cv);
        }
        return 0;
    }
    }
    lg("unhandled DRM ioctl nr 0x%02lx (req 0x%08lx)", (unsigned long)_IOC_NR(req), req);
    return -EINVAL;
}

/* ---- interposed libc ------------------------------------------------------- */

static int is_card(const char *path) {
    return path && strncmp(path, "/dev/dri/card", 13) == 0;
}

static int open_fake(const char *path, int flags) {
    pthread_mutex_lock(&lk);
    if (fake_rd >= 0) {
        /* One app, one device: a second open shares the first's state. */
        int d = fcntl(fake_rd, (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
        pthread_mutex_unlock(&lk);
        lg("open %s again -> fd %d", path, d);
        return d;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) { pthread_mutex_unlock(&lk); return -1; }
    if (!(flags & O_CLOEXEC)) fcntl(p[0], F_SETFD, 0);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    fake_rd = p[0]; fake_wr = p[1];
    static int thread_started;
    if (!thread_started) {
        pthread_t t; pthread_create(&t, NULL, vblank_thread, NULL); pthread_detach(t);
        thread_started = 1;
    }
    pthread_mutex_unlock(&lk);
    lg("open %s -> fake fd %d, mode %ux%u", path, fake_rd, mode_w, mode_h);
    return fake_rd;
}

int open(const char *path, int flags, ...) {
    if (is_card(path)) return open_fake(path, flags);
    va_list ap; va_start(ap, flags); mode_t m = va_arg(ap, mode_t); va_end(ap);
    return real_open(path, flags, m);
}
int open64(const char *path, int flags, ...) __attribute__((alias("open")));
int __open_2(const char *path, int flags) { return open(path, flags, 0); }
int __open64_2(const char *path, int flags) { return open(path, flags, 0); }

int openat(int dirfd, const char *path, int flags, ...) {
    if (is_card(path)) return open_fake(path, flags);
    va_list ap; va_start(ap, flags); mode_t m = va_arg(ap, mode_t); va_end(ap);
    return real_openat(dirfd, path, flags, m);
}
int openat64(int dirfd, const char *path, int flags, ...) __attribute__((alias("openat")));
int __openat_2(int dirfd, const char *path, int flags) { return openat(dirfd, path, flags, 0); }

static int is_fake(int fd) {
    if (fd < 0 || fake_rd < 0) return 0;
    if (fd == fake_rd) return 1;
    /* Duplicates of it, from a second open. */
    struct stat a, b;
    return fstat(fd, &a) == 0 && fstat(fake_rd, &b) == 0 &&
           a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (_IOC_TYPE(req) == DRM_IOCTL_BASE && is_fake(fd)) {
        pthread_mutex_lock(&lk);
        int r = drm_ioctl(req, arg);
        pthread_mutex_unlock(&lk);
        if (r < 0) { errno = -r; return -1; }
        return 0;
    }
    return real_ioctl(fd, req, arg);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (is_fake(fd)) {
        uint32_t h = (uint32_t)((uint64_t)off >> MAP_SHIFT);
        pthread_mutex_lock(&lk);
        int mfd = (h < MAX_BUFS && bufs[h].used) ? bufs[h].memfd : -1;
        pthread_mutex_unlock(&lk);
        if (mfd < 0) { errno = EINVAL; return MAP_FAILED; }
        return real_mmap(addr, len, prot, flags, mfd, (off_t)((uint64_t)off & ((1ull << MAP_SHIFT) - 1)));
    }
    return real_mmap(addr, len, prot, flags, fd, off);
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) __attribute__((alias("mmap")));

/* Input. MPC takes its touchscreen, keyboards and mice through libinput, which
 * keeps the devices whose udev ID_SEAT is the seat it was given, seat0, the
 * same seat the compositor uses. kmspresent's touchscreen must reach MPC and
 * not the compositor, which would otherwise feed each touch back to the window,
 * and the host's own keyboard and mice must reach the compositor and not MPC.
 * So the touchscreen is put on a seat of its own on the host (a udev rule, see
 * chroot-mpc.sh), and MPC alone is told that seat is seat0 and that every other
 * device is somewhere else. libinput reads the seat with this call. */
const char *udev_device_get_property_value(void *dev, const char *key) {
    static const char *(*real)(void *, const char *);
    if (!real) real = dlsym(RTLD_NEXT, "udev_device_get_property_value");
    const char *v = real ? real(dev, key) : NULL;
    const char *seat = getenv("KMSSHIM_SEAT");
    if (seat && *seat && key && strcmp(key, "ID_SEAT") == 0)
        return v && strcmp(v, seat) == 0 ? "seat0" : "kmsshim-host";
    return v;
}

int close(int fd) {
    if (fd >= 0 && fd == fake_rd) {
        pthread_mutex_lock(&lk);
        lg("close fake fd %d", fd);
        real_close(fake_wr); fake_wr = -1; fake_rd = -1; flip_pending = 0;
        if (sock >= 0) { real_close(sock); sock = -1; }
        pthread_mutex_unlock(&lk);
    }
    return real_close(fd);
}
