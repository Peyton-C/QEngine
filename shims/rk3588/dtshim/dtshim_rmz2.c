/* Forked from dtshim.c for RANE SYSTEM ONE (RMZ2, arm64/RK3588) — see
 * BUILDING.md's arm64/RK3588 section. Devicetree layout differs from the
 * RK3288 az01 lineup this file was originally written for:
 *   - product-code value is "RMZ2", not JC11/JP11
 *   - no inmusic,az01-pcb-rev property exists in RMZ2's devicetree
 *   - the panel/rotation node lives at dsi@fde20000/panel@0, not mipi@ff960000
 *   - no chosen/inmusic,internal-sd-fitted property found
 *   - the Qt5-ABI stub symbols in the original file are gone: this build is
 *     Qt 6.7.2, a different ABI, and that missing-symbol issue was never
 *     observed here
 *   - the original file's eglCreateWindowSurface/eglSurfaceAttrib
 *     interception (a workaround for a RK3288-era fbdev_window/single-buffer
 *     quirk) is gone too: on this build it actively broke KMS scanout —
 *     eglSwapBuffers kept succeeding but nothing ever reached the screen,
 *     because forcing EGL_SINGLE_BUFFER and re-wrapping the window handle in
 *     a second, shim-created gbm_surface fights Qt6's own eglfs-kms-gbm
 *     backend, which already creates and tracks its own GBM surface/pageflip
 *     correctly on its own. Removing it fixed the blank-screen symptom.
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
    if (strcmp(path, "/dev/mem") == 0)
        return "/root/fake-dev-mem";
    if (strcmp(path, "/sys/firmware/devicetree/base/dsi@fde20000/panel@0/rotation") == 0)
        return "/root/fake-dt/rotation";
    /* Engine hard-throws (uncaught std::runtime_error, aborts the process)
     * if it can't find a real-hardware "dwc3" (USB3 controller) IRQ line to
     * pin CPU affinity for. QEMU's virt machine has no such controller.
     * Fake file reuses real IRQ numbers already present in the guest under
     * fake names, so the CPU-affinity write Engine does right after also
     * lands on a real /proc/irq/<N>/ directory instead of failing too. */
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