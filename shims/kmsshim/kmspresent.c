/*
 * kmspresent.c — shows what an app running on kmsshim puts on its "screen"
 * as a window in a Wayland compositor, and gives the app a touchscreen.
 *
 * Runs on the host, as root, beside the chroot. It listens on a unix socket
 * the app's kmsshim connects to (inside the chroot's /run), receives each dumb
 * buffer as a memfd and each framebuffer's layout, and attaches whichever one
 * the app flips to as a wl_shm buffer: no copy. The window can be any size;
 * wp_viewporter scales the picture to it, and input is scaled back.
 *
 * Input: MPC reads its touchscreen from evdev and looks for it once, as it
 * starts. kmspresent therefore creates a uinput multitouch device before the
 * app is started, and feeds it the window's touches, and the left mouse button
 * as a touch. A udev rule (see chroot-mpc.sh) keeps libinput, and so the
 * compositor, from taking that device as a touchscreen of its own.
 *
 * Usage: kmspresent [--socket PATH] [--size WxH] [--title TEXT]
 *   --socket  where to listen. Default /srv/mpc/run/kmsshim.sock
 *   --size    the app's screen, for the touchscreen's range. Default 1280x800
 * WAYLAND_DISPLAY and XDG_RUNTIME_DIR name the compositor, as for any client.
 *
 * Build: wayland-scanner client-header/private-code for xdg-shell and viewporter,
 * then gcc -O2 -o kmspresent kmspresent.c xdg-shell.c viewporter.c -lwayland-client
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "kmsproto.h"

#define MAX_BUFS 256
#define MAX_FBS 512
#define FB_BASE 100
#define SLOTS 10

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm;
static struct wp_viewporter *vporter;
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static struct wp_viewport *vport;
static struct wl_callback *frame_cb;
static int configured, win_w, win_h;   /* the surface's size: the app's aspect, fitted in the tile */
static int app_w = 1280, app_h = 800;

static struct { struct wl_shm_pool *pool; } bufs[MAX_BUFS];
static struct { struct wl_buffer *wb; int w, h; } fbs[MAX_FBS];
static int want_fb = -1, shown_fb = -1;
static int dmg_x1, dmg_y1, dmg_x2, dmg_y2, dmg_full = 1;

static int ui = -1;               /* uinput touchscreen */
static int slot_id[SLOTS];        /* wl_touch id per slot, or -1 */
static int next_tracking;
static int ptr_down; static double ptr_x, ptr_y;

/* ---- the touchscreen --------------------------------------------------------- */

static void emit(int type, int code, int val) {
    struct input_event e = {0};
    e.type = type; e.code = code; e.value = val;
    if (write(ui, &e, sizeof e) != sizeof e) perror("uinput write");
}

static void make_touchscreen(void) {
    ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ui < 0) { perror("/dev/uinput"); exit(1); }
    ioctl(ui, UI_SET_EVBIT, EV_KEY); ioctl(ui, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(ui, UI_SET_EVBIT, EV_ABS);
    ioctl(ui, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    struct uinput_abs_setup a = {0};
    int codes[] = {ABS_X, ABS_Y, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_SLOT, ABS_MT_TRACKING_ID};
    for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++) {
        memset(&a, 0, sizeof a);
        a.code = codes[i];
        switch (codes[i]) {
        case ABS_X: case ABS_MT_POSITION_X: a.absinfo.maximum = app_w - 1; break;
        case ABS_Y: case ABS_MT_POSITION_Y: a.absinfo.maximum = app_h - 1; break;
        case ABS_MT_SLOT: a.absinfo.maximum = SLOTS - 1; break;
        case ABS_MT_TRACKING_ID: a.absinfo.maximum = 65535; break;
        }
        ioctl(ui, UI_SET_ABSBIT, codes[i]);
        if (ioctl(ui, UI_ABS_SETUP, &a) < 0) perror("UI_ABS_SETUP");
    }
    struct uinput_setup s = {0};
    s.id.bustype = BUS_VIRTUAL; s.id.vendor = 0x5145; s.id.product = 0x4b4d;
    snprintf(s.name, sizeof s.name, "QEngine kmspresent touchscreen");
    if (ioctl(ui, UI_DEV_SETUP, &s) < 0 || ioctl(ui, UI_DEV_CREATE) < 0) {
        perror("uinput create"); exit(1);
    }
    for (int i = 0; i < SLOTS; i++) slot_id[i] = -1;
    fprintf(stderr, "touchscreen %dx%d created\n", app_w, app_h);
}

static int to_app_x(double x) { int v = win_w ? (int)(x * app_w / win_w) : (int)x; return v < 0 ? 0 : v >= app_w ? app_w - 1 : v; }
static int to_app_y(double y) { int v = win_h ? (int)(y * app_h / win_h) : (int)y; return v < 0 ? 0 : v >= app_h ? app_h - 1 : v; }

static int active_slots(void) { int n = 0; for (int i = 0; i < SLOTS; i++) n += slot_id[i] >= 0; return n; }

static void touch_down(int slot, double x, double y) {
    int was = active_slots();
    slot_id[slot] = 1;
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_TRACKING_ID, next_tracking++ & 0xffff);
    emit(EV_ABS, ABS_MT_POSITION_X, to_app_x(x)); emit(EV_ABS, ABS_MT_POSITION_Y, to_app_y(y));
    if (!was) { emit(EV_KEY, BTN_TOUCH, 1); emit(EV_ABS, ABS_X, to_app_x(x)); emit(EV_ABS, ABS_Y, to_app_y(y)); }
    emit(EV_SYN, SYN_REPORT, 0);
}
static void touch_move(int slot, double x, double y) {
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_POSITION_X, to_app_x(x)); emit(EV_ABS, ABS_MT_POSITION_Y, to_app_y(y));
    emit(EV_ABS, ABS_X, to_app_x(x)); emit(EV_ABS, ABS_Y, to_app_y(y));
    emit(EV_SYN, SYN_REPORT, 0);
}
static void touch_up(int slot) {
    slot_id[slot] = -1;
    emit(EV_ABS, ABS_MT_SLOT, slot);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    if (!active_slots()) emit(EV_KEY, BTN_TOUCH, 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

/* ---- Wayland input -------------------------------------------------------- */

static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *w, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)s; (void)w;
    /* No cursor of our own is set, so the compositor's stays: the app draws none. */
    ptr_x = wl_fixed_to_double(x); ptr_y = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *w) {
    (void)d; (void)p; (void)s; (void)w;
    if (ptr_down) { touch_up(SLOTS - 1); ptr_down = 0; }
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t;
    ptr_x = wl_fixed_to_double(x); ptr_y = wl_fixed_to_double(y);
    if (ptr_down) touch_move(SLOTS - 1, ptr_x, ptr_y);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
    (void)d; (void)p; (void)s; (void)t;
    if (b != BTN_LEFT) return;
    if (st == WL_POINTER_BUTTON_STATE_PRESSED && !ptr_down) { ptr_down = 1; touch_down(SLOTS - 1, ptr_x, ptr_y); }
    else if (st == WL_POINTER_BUTTON_STATE_RELEASED && ptr_down) { ptr_down = 0; touch_up(SLOTS - 1); }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_l = {p_enter, p_leave, p_motion, p_button, p_axis,
                                                 p_frame, p_axis_source, p_axis_stop, p_axis_discrete};

static int slot_for(int32_t id, int alloc) {
    for (int i = 0; i < SLOTS - 1; i++) if (slot_id[i] == id + 1000) return i;
    if (!alloc) return -1;
    for (int i = 0; i < SLOTS - 1; i++) if (slot_id[i] < 0) return i;
    return -1;
}
static void t_down(void *d, struct wl_touch *t, uint32_t s, uint32_t tm, struct wl_surface *w, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)t; (void)s; (void)tm; (void)w;
    int sl = slot_for(id, 1);
    if (sl < 0) return;
    touch_down(sl, wl_fixed_to_double(x), wl_fixed_to_double(y));
    slot_id[sl] = id + 1000;
}
static void t_up(void *d, struct wl_touch *t, uint32_t s, uint32_t tm, int32_t id) {
    (void)d; (void)t; (void)s; (void)tm;
    int sl = slot_for(id, 0); if (sl >= 0) touch_up(sl);
}
static void t_motion(void *d, struct wl_touch *t, uint32_t tm, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)t; (void)tm;
    int sl = slot_for(id, 0); if (sl >= 0) touch_move(sl, wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void t_frame(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void t_cancel(void *d, struct wl_touch *t) {
    (void)d; (void)t;
    for (int i = 0; i < SLOTS - 1; i++) if (slot_id[i] >= 0) touch_up(i);
}
static void t_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t a, wl_fixed_t b) { (void)d; (void)t; (void)id; (void)a; (void)b; }
static void t_orient(void *d, struct wl_touch *t, int32_t id, wl_fixed_t o) { (void)d; (void)t; (void)id; (void)o; }
static const struct wl_touch_listener touch_l = {t_down, t_up, t_motion, t_frame, t_cancel, t_shape, t_orient};

static struct wl_pointer *pointer; static struct wl_touch *touch;
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s); wl_pointer_add_listener(pointer, &ptr_l, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !touch) {
        touch = wl_seat_get_touch(s); wl_touch_add_listener(touch, &touch_l, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = {seat_caps, seat_name};

/* ---- the window ------------------------------------------------------------ */

static void show_pending(void);

static void frame_done(void *d, struct wl_callback *cb, uint32_t t) {
    (void)d; (void)t;
    wl_callback_destroy(cb); frame_cb = NULL;
    show_pending();
}
static const struct wl_callback_listener frame_l = {frame_done};

static void set_view(void) {
    if (vport) wp_viewport_set_destination(vport, win_w ? win_w : app_w, win_h ? win_h : app_h);
}

/* Attach the newest framebuffer the app flipped to, at most once per frame the
 * compositor draws; flips in between are skipped, as a slow monitor would. */
static void show_pending(void) {
    if (!configured || frame_cb || want_fb < 0 || !fbs[want_fb].wb) return;
    if (want_fb == shown_fb && !dmg_full && dmg_x2 <= dmg_x1) return;
    wl_surface_attach(surf, fbs[want_fb].wb, 0, 0);
    if (dmg_full || want_fb != shown_fb) wl_surface_damage_buffer(surf, 0, 0, INT32_MAX, INT32_MAX);
    else wl_surface_damage_buffer(surf, dmg_x1, dmg_y1, dmg_x2 - dmg_x1, dmg_y2 - dmg_y1);
    dmg_full = 0; dmg_x1 = dmg_y1 = 1 << 30; dmg_x2 = dmg_y2 = -1;
    shown_fb = want_fb;
    frame_cb = wl_surface_frame(surf);
    wl_callback_add_listener(frame_cb, &frame_l, NULL);
    wl_surface_commit(surf);
}

static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(x, serial);
    configured = 1;
    set_view();
    dmg_full = 1; shown_fb = -1;
    if (want_fb >= 0) show_pending();
    else wl_surface_commit(surf);
}
static const struct xdg_surface_listener xs_l = {xs_configure};

/* The picture keeps the app's aspect ratio: the surface is the largest box of
 * that shape inside what the compositor offers, and a tiling compositor leaves
 * the rest of the tile empty. MPC's touch targets are sized for its own screen,
 * and a stretched picture would put them out of proportion too. */
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)t; (void)st;
    if (w <= 0 || h <= 0 || !vport) { win_w = app_w; win_h = app_h; return; }  /* no viewporter: 1:1 */
    if ((long)w * app_h > (long)h * app_w) { win_h = h; win_w = (int)((long)h * app_w / app_h); }
    else { win_w = w; win_h = (int)((long)w * app_h / app_w); }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = {top_configure, top_close, top_bounds, top_caps};

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { (void)d; xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_l = {wm_ping};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(seat, &seat_l, NULL);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm, &wm_l, NULL);
    } else if (!strcmp(iface, wp_viewporter_interface.name))
        vporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

/* ---- the app's side --------------------------------------------------------- */

static void forget_app(void) {
    for (int i = 0; i < MAX_FBS; i++) if (fbs[i].wb) { wl_buffer_destroy(fbs[i].wb); fbs[i].wb = NULL; }
    for (int i = 0; i < MAX_BUFS; i++) if (bufs[i].pool) { wl_shm_pool_destroy(bufs[i].pool); bufs[i].pool = NULL; }
    want_fb = shown_fb = -1;
}

static void handle(const struct kmsmsg *m, int fd) {
    switch (m->type) {
    case KMS_MODE:
        fprintf(stderr, "app screen %ux%u\n", m->w, m->h);
        if ((int)m->w != app_w || (int)m->h != app_h)
            fprintf(stderr, "warning: touchscreen is %dx%d; restart kmspresent with --size %ux%u\n",
                    app_w, app_h, m->w, m->h);
        break;
    case KMS_BUF:
        if (m->id >= MAX_BUFS || fd < 0) break;
        if (bufs[m->id].pool) wl_shm_pool_destroy(bufs[m->id].pool);
        bufs[m->id].pool = wl_shm_create_pool(shm, fd, (int32_t)m->size);
        break;
    case KMS_FREEBUF:
        if (m->id < MAX_BUFS && bufs[m->id].pool) { wl_shm_pool_destroy(bufs[m->id].pool); bufs[m->id].pool = NULL; }
        break;
    case KMS_FB: {
        unsigned i = m->id - FB_BASE;
        if (i >= MAX_FBS || m->buf >= MAX_BUFS || !bufs[m->buf].pool) break;
        if (fbs[i].wb) wl_buffer_destroy(fbs[i].wb);
        /* wl_shm uses DRM fourccs, except for these two, which it numbers 0 and 1. */
        uint32_t f = m->format == 0x34325258 ? WL_SHM_FORMAT_XRGB8888 :
                     m->format == 0x34325241 ? WL_SHM_FORMAT_ARGB8888 : m->format;
        fbs[i].wb = wl_shm_pool_create_buffer(bufs[m->buf].pool, m->offset, m->w, m->h, m->pitch, f);
        fbs[i].w = m->w; fbs[i].h = m->h;
        break;
    }
    case KMS_RMFB: {
        unsigned i = m->id - FB_BASE;
        if (i >= MAX_FBS || !fbs[i].wb) break;
        /* Still on screen: keep it until something replaces it. A wl_buffer
         * destroyed while attached is fine in the protocol, but shows nothing. */
        if ((int)i == shown_fb || (int)i == want_fb) break;
        wl_buffer_destroy(fbs[i].wb); fbs[i].wb = NULL;
        break;
    }
    case KMS_SHOW: {
        unsigned i = m->id - FB_BASE;
        if (i >= MAX_FBS || !fbs[i].wb) break;
        if (fbs[i].w != app_w || fbs[i].h != app_h) break;  /* not a whole screen: not ours to show */
        if ((int)i != want_fb) dmg_full = 1;
        want_fb = i;
        if (m->x1 < dmg_x1) dmg_x1 = m->x1;
        if (m->y1 < dmg_y1) dmg_y1 = m->y1;
        if (m->x2 > dmg_x2) dmg_x2 = m->x2;
        if (m->y2 > dmg_y2) dmg_y2 = m->y2;
        show_pending();
        break;
    }
    }
    if (fd >= 0) close(fd);
}

static int recv_msg(int s, struct kmsmsg *m, int *fd) {
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {m, sizeof *m};
    struct msghdr mh = {0};
    mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    ssize_t n = recvmsg(s, &mh, MSG_CMSG_CLOEXEC);
    if (n <= 0) return -1;
    *fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof(int));
    if (n != sizeof *m) { if (*fd >= 0) close(*fd); *fd = -1; m->type = 0; }
    return 0;
}

int main(int argc, char **argv) {
    const char *path = "/srv/mpc/run/kmsshim.sock", *title = "MPC";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &app_w, &app_h) != 2) { fprintf(stderr, "bad --size\n"); return 1; }
        } else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else { fprintf(stderr, "usage: %s [--socket PATH] [--size WxH] [--title TEXT]\n", argv[0]); return 1; }
    }
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);

    dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no Wayland compositor (WAYLAND_DISPLAY, XDG_RUNTIME_DIR)\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "compositor lacks wl_compositor, wl_shm or xdg_wm_base\n"); return 1; }
    wl_display_roundtrip(dpy);

    make_touchscreen();

    surf = wl_compositor_create_surface(comp);
    if (vporter) vport = wp_viewporter_get_viewport(vporter, surf);
    xsurf = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xsurf, &xs_l, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, title);
    xdg_toplevel_set_app_id(top, "qengine-kmspresent");
    wl_surface_commit(surf);

    unlink(path);
    int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 2) < 0) { perror(path); return 1; }
    chmod(path, 0666);
    fprintf(stderr, "listening on %s\n", path);
    int cs = -1;

    for (;;) {
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct pollfd p[3] = {{wl_display_get_fd(dpy), POLLIN, 0}, {ls, POLLIN, 0}, {cs, POLLIN, 0}};
        if (poll(p, cs >= 0 ? 3 : 2, -1) < 0 && errno != EINTR) { wl_display_cancel_read(dpy); break; }
        if (p[0].revents & POLLIN) { if (wl_display_read_events(dpy) < 0) break; }
        else wl_display_cancel_read(dpy);
        if (p[0].revents & (POLLERR | POLLHUP)) { fprintf(stderr, "compositor went away\n"); break; }
        if (wl_display_dispatch_pending(dpy) < 0) break;

        if (p[1].revents & POLLIN) {
            int n = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
            if (n >= 0) {
                if (cs >= 0) { close(cs); forget_app(); }
                cs = n; fprintf(stderr, "app connected\n");
            }
        }
        if (cs >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            for (;;) {
                struct kmsmsg m; int fd;
                if (recv_msg(cs, &m, &fd) < 0) {
                    fprintf(stderr, "app disconnected\n");
                    close(cs); cs = -1; forget_app();
                    break;
                }
                handle(&m, fd);
                struct pollfd more = {cs, POLLIN, 0};
                if (poll(&more, 1, 0) <= 0) break;
            }
        }
    }
    return 0;
}
