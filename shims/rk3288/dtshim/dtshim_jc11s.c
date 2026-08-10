/* RK3288/armv7 dtshim for Engine 5.0.4 (Qt 6.7.2, eglfs) — a from-scratch
 * rewrite of shims/rk3288/dtshim/dtshim.c for this Qt6 build, not a port of
 * it: that file's EGL/GBM interception (eglCreateWindowSurface wrapping a
 * gbm_surface, forcing EGL_SINGLE_BUFFER) and Qt5Gui ABI stub symbols
 * (QPlatformCursor::setOverrideCursor etc.) were both specific to the old
 * custom Qt5 build's quirks. We already learned on RK3588/RMZ2
 * (shims/rk3588/dtshim/dtshim_rmz2.c) that this exact kind of EGL/GBM
 * interception actively breaks Qt6's eglfs-kms-gbm backend, which already
 * creates and tracks its own GBM surface/pageflip correctly on its own —
 * so this file only keeps the open/fopen path-remapping mechanism, with
 * RK3288's real devicetree paths (unchanged hardware, confirmed still
 * correct against Engine 5.0.4's shared/universal /usr/Engine tree) plus
 * a new /proc/interrupts remap for the IRQ-affinity check discovered
 * during the RK3588/RMZ2 bring-up (BUILDING.md's arm64 section) — unknown
 * yet whether Engine 5.0.4 does the same check on this hardware; added
 * defensively, fake file to be filled in once/if a crash confirms it's
 * needed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <errno.h>

static const char *remap(const char *path) {
    if (!path) return NULL;
    if (strcmp(path, "/sys/firmware/devicetree/base/inmusic,product-code") == 0)
        return "/root/fake-dt/inmusic,product-code";
    if (strcmp(path, "/sys/firmware/devicetree/base/serial-number") == 0)
        return "/root/fake-dt/serial-number";
    if (strcmp(path, "/sys/firmware/devicetree/base/inmusic,az01-pcb-rev") == 0)
        return "/root/fake-dt/inmusic,az01-pcb-rev";
    if (strcmp(path, "/dev/mem") == 0)
        return "/root/fake-dev-mem";
    if (strcmp(path, "/sys/firmware/devicetree/base/mipi@ff960000/panel@0/rotation") == 0)
        return "/root/fake-dt/rotation";
    if (strcmp(path, "/sys/firmware/devicetree/base/chosen/inmusic,internal-sd-fitted") == 0)
        return "/root/fake-dt/inmusic,internal-sd-fitted";
    if (strcmp(path, "/proc/interrupts") == 0)
        return "/root/fake-dt/interrupts";
    return path;
}

typedef int (*open_t)(const char *, int, ...);
typedef int (*open64_t)(const char *, int, ...);
typedef FILE *(*fopen_t)(const char *, const char *);
typedef FILE *(*fopen64_t)(const char *, const char *);

int open(const char *path, int flags, ...) {
    static open_t real_open = NULL;
    if (!real_open) real_open = (open_t)dlsym(RTLD_NEXT, "open");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);
    return real_open(remap(path), flags, mode);
}

int open64(const char *path, int flags, ...) {
    static open64_t real_open64 = NULL;
    if (!real_open64) real_open64 = (open64_t)dlsym(RTLD_NEXT, "open64");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);
    return real_open64(remap(path), flags, mode);
}

FILE *fopen(const char *path, const char *mode) {
    static fopen_t real_fopen = NULL;
    if (!real_fopen) real_fopen = (fopen_t)dlsym(RTLD_NEXT, "fopen");
    return real_fopen(remap(path), mode);
}

FILE *fopen64(const char *path, const char *mode) {
    static fopen64_t real_fopen64 = NULL;
    if (!real_fopen64) real_fopen64 = (fopen64_t)dlsym(RTLD_NEXT, "fopen64");
    return real_fopen64(remap(path), mode);
}