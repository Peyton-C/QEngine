/* Draws a mouse pointer over Engine, on the display's DRM cursor plane.
 *
 * Engine is a touch application and shows no pointer. On hardware that is
 * right. Under emulation the "finger" is a host mouse, and a display that hides
 * the host pointer (UTM does; QEMU's cocoa and VNC displays do not) leaves
 * nothing on screen to aim with.
 *
 * Qt's own cursor is not used, because it cannot be made to appear here. Engine
 * hides it (it sets QT_QPA_EGLFS_HIDECURSOR itself, before Qt starts), and
 * undoing that is not enough: with the variable refused, a mouse device present
 * and the cursor image set loaded, eglfs still never put an image on the cursor
 * plane, and never once asked for a cursor shape. Qt only shows its cursor once
 * something applies one to a window, and nothing in Engine does.
 *
 * So this owns the plane instead and leaves Qt's cursor disabled, which also
 * means the two can never fight over it. A thread in Engine's process:
 *   1. waits for Engine to open the display and light a CRTC,
 *   2. puts a 64x64 arrow on that CRTC's cursor plane, using Engine's own DRM
 *      file descriptor -- cursor ioctls need the DRM master, and that is Engine,
 *   3. follows the motion-only device that `touchbridge --pointer` publishes.
 *      The tablet itself cannot be read: touchbridge holds it exclusively so
 *      that Engine sees touch and nothing else.
 *
 * Preloaded into everything engine.service starts, like the other shims, but it
 * only does anything in the Engine process itself.
 *
 * Env vars:
 *   CURSORSHIM_DEBUG  non-empty to log progress to stderr
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/input.h>
#include <drm.h>
#include <drm_mode.h>

#define CURSOR_SIZE 64
#define POINTER_NAME "TouchBridge Virtual Pointer"

static int debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("CURSORSHIM_DEBUG") != NULL;
    return cached;
}

#define dbg(...) do { if (debug_enabled()) { \
    fprintf(stderr, "=== CURSORSHIM: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* Straight to the kernel. drmatomic interposes ioctl() in this same process and
 * has no business seeing these, and going around it keeps the two independent. */
static int raw_ioctl(int fd, unsigned long request, void *arg) {
    int ret;
    do {
        ret = (int)syscall(SYS_ioctl, fd, request, arg);
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

/* The classic arrow, hotspot at the top-left tip. '#' is outline, '.' is fill. */
static const char *const ARROW[] = {
    "#",
    "##",
    "#.#",
    "#..#",
    "#...#",
    "#....#",
    "#.....#",
    "#......#",
    "#.......#",
    "#........#",
    "#.........#",
    "#..........#",
    "#......#####",
    "#...#..#",
    "#..##..#",
    "#.#  #..#",
    "##   #..#",
    "#     #..#",
    "      #..#",
    "       ##",
};

static void draw_arrow(uint32_t *px, uint32_t pitch_px) {
    for (size_t row = 0; row < sizeof(ARROW) / sizeof(ARROW[0]); row++)
        for (size_t col = 0; ARROW[row][col]; col++) {
            char c = ARROW[row][col];
            if (c == '#') px[row * pitch_px + col] = 0xFF000000u;
            else if (c == '.') px[row * pitch_px + col] = 0xFFFFFFFFu;
        }
}

/* Engine's DRM device, found among the descriptors it already has open. */
static int find_drm_fd(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        char link[300], target[128];
        snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        if (strncmp(target, "/dev/dri/card", 13) == 0) {
            found = atoi(e->d_name);
            break;
        }
    }
    closedir(d);
    return found;
}

/* The first CRTC with a mode set, or 0 while Engine has not lit one yet. */
static uint32_t find_active_crtc(int fd) {
    uint32_t crtcs[8];
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.count_crtcs = sizeof(crtcs) / sizeof(crtcs[0]);
    if (raw_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return 0;
    uint32_t n = res.count_crtcs;
    if (n > sizeof(crtcs) / sizeof(crtcs[0])) n = sizeof(crtcs) / sizeof(crtcs[0]);
    for (uint32_t i = 0; i < n; i++) {
        struct drm_mode_crtc crtc;
        memset(&crtc, 0, sizeof(crtc));
        crtc.crtc_id = crtcs[i];
        if (raw_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0 && crtc.mode_valid && crtc.fb_id)
            return crtcs[i];
    }
    return 0;
}

/* A 64x64 ARGB buffer holding the arrow; returns its GEM handle, or 0. */
static uint32_t create_cursor_buffer(int fd) {
    struct drm_mode_create_dumb create;
    memset(&create, 0, sizeof(create));
    create.width = CURSOR_SIZE;
    create.height = CURSOR_SIZE;
    create.bpp = 32;
    if (raw_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        dbg("CREATE_DUMB failed: %s", strerror(errno));
        return 0;
    }
    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (raw_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        dbg("MAP_DUMB failed: %s", strerror(errno));
        return 0;
    }
    uint32_t *px = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (px == MAP_FAILED) {
        dbg("mmap of the cursor buffer failed: %s", strerror(errno));
        return 0;
    }
    memset(px, 0, create.size);
    draw_arrow(px, create.pitch / 4);
    munmap(px, create.size);
    return create.handle;
}

static int open_pointer_device(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while (found < 0 && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[300], name[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            strcmp(name, POINTER_NAME) == 0) {
            dbg("following %s (%s)", path, name);
            found = fd;
        } else {
            close(fd);
        }
    }
    closedir(d);
    return found;
}

static void *cursor_thread(void *unused) {
    (void)unused;

    int drm = -1;
    uint32_t crtc = 0;
    while (!crtc) {
        sleep(1);
        if (drm < 0) drm = find_drm_fd();
        if (drm >= 0) crtc = find_active_crtc(drm);
    }
    dbg("display fd %d, crtc %u", drm, crtc);

    uint32_t handle = create_cursor_buffer(drm);
    if (!handle) return NULL;

    struct drm_mode_cursor cur;
    memset(&cur, 0, sizeof(cur));
    cur.flags = DRM_MODE_CURSOR_BO;
    cur.crtc_id = crtc;
    cur.width = CURSOR_SIZE;
    cur.height = CURSOR_SIZE;
    cur.handle = handle;
    if (raw_ioctl(drm, DRM_IOCTL_MODE_CURSOR, &cur) < 0) {
        dbg("setting the cursor image failed: %s", strerror(errno));
        return NULL;
    }
    dbg("cursor image set");

    for (;;) {
        int in = open_pointer_device();
        if (in < 0) {
            /* touchbridge not started with --pointer, or not started yet. */
            sleep(2);
            continue;
        }
        int x = 0, y = 0;
        struct input_event ev;
        while (read(in, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_ABS && ev.code == ABS_X) x = ev.value;
            else if (ev.type == EV_ABS && ev.code == ABS_Y) y = ev.value;
            else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                memset(&cur, 0, sizeof(cur));
                cur.flags = DRM_MODE_CURSOR_MOVE;
                cur.crtc_id = crtc;
                cur.x = x;
                cur.y = y;
                raw_ioctl(drm, DRM_IOCTL_MODE_CURSOR, &cur);
            }
        }
        /* The device went away (touchbridge restarted); look for it again. */
        close(in);
        sleep(1);
    }
    return NULL;
}

__attribute__((constructor))
static void cursorshim_init(void) {
    /* LD_PRELOAD reaches every process the service starts -- shells, cat,
     * fscryptctl. Only Engine has a display to draw on. */
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = '\0';
    const char *base = strrchr(exe, '/');
    if (!base || strcmp(base + 1, "Engine") != 0) return;

    pthread_t t;
    if (pthread_create(&t, NULL, cursor_thread, NULL) == 0) pthread_detach(t);
}
