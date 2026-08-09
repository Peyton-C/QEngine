#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <errno.h>

/* Stub for a QPlatformCursor private-API method missing from Denon's older/custom
 * Qt5Gui build, needed only to satisfy the linker when loading Debian's stock
 * libQt5XcbQpa.so. No-op is fine: worst case, cursor-shape override is a no-op. */
void _ZN15QPlatformCursor17setOverrideCursorERK7QCursor(void *this_ptr, const void *cursor_ref) {
    (void)this_ptr;
    (void)cursor_ref;
}

void _ZN15QPlatformCursor19clearOverrideCursorEv(void *this_ptr) {
    (void)this_ptr;
}

/* QString sessionId() const - non-trivial return type uses ARM's hidden
 * return-pointer ABI: r0=return slot, r1=this. QString is a single pointer
 * (d-ptr) internally; zeroing it represents an empty/null string safely. */
void *_ZNK23QPlatformSessionManager9sessionIdEv(void *return_slot, const void *this_ptr) {
    (void)this_ptr;
    *(void **)return_slot = 0;
    return return_slot;
}

void *_ZNK23QPlatformSessionManager10sessionKeyEv(void *return_slot, const void *this_ptr) {
    (void)this_ptr;
    *(void **)return_slot = 0;
    return return_slot;
}

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

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef long EGLNativeWindowType;
typedef int EGLint;
typedef EGLSurface (*eglCreateWindowSurface_t)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);

struct gbm_device;
struct gbm_surface;
typedef struct gbm_device *(*gbm_create_device_t)(int);
typedef struct gbm_surface *(*gbm_surface_create_t)(struct gbm_device *, unsigned, unsigned, unsigned, unsigned);

#define GBM_FORMAT_XRGB8888 0x34325258
#define GBM_BO_USE_SCANOUT (1 << 0)
#define GBM_BO_USE_RENDERING (1 << 2)

static struct gbm_device *get_gbm_device(void) {
    static struct gbm_device *dev = NULL;
    static int tried = 0;
    if (tried) return dev;
    tried = 1;

    void *libgbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!libgbm) {
        fprintf(stderr, "=== SHIM: dlopen(libgbm.so.1) failed: %s ===\n", dlerror());
        return NULL;
    }
    gbm_create_device_t create_device = (gbm_create_device_t)dlsym(libgbm, "gbm_create_device");
    if (!create_device) {
        fprintf(stderr, "=== SHIM: dlsym(gbm_create_device) failed ===\n");
        return NULL;
    }
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "=== SHIM: open(/dev/dri/card0) failed: %s ===\n", strerror(errno));
        return NULL;
    }
    dev = create_device(fd);
    if (!dev) {
        fprintf(stderr, "=== SHIM: gbm_create_device failed ===\n");
    }
    return dev;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list) {
    static eglCreateWindowSurface_t real_fn = NULL;
    if (!real_fn) real_fn = (eglCreateWindowSurface_t)dlsym(RTLD_NEXT, "eglCreateWindowSurface");
    fprintf(stderr, "=== SHIM: eglCreateWindowSurface(dpy=%p, config=%p, win=0x%lx, attribs=%p) ===\n",
            dpy, config, win, (void *)attrib_list);

    EGLNativeWindowType actual_win = win;

    if (win > 0x1000 && win < 0x80000000L && !getenv("DTSHIM_NO_GBM")) {
        unsigned int *w = (unsigned int *)win;
        unsigned width = w[0] & 0xffff;
        unsigned height = (w[0] >> 16) & 0xffff;
        fprintf(stderr, "=== SHIM: fbdev_window-like struct: width=%u height=%u ===\n", width, height);

        if (width > 0 && width < 8192 && height > 0 && height < 8192) {
            struct gbm_device *gbm = get_gbm_device();
            if (gbm) {
                static gbm_surface_create_t surface_create = NULL;
                if (!surface_create) {
                    void *libgbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
                    surface_create = (gbm_surface_create_t)dlsym(libgbm, "gbm_surface_create");
                }

                unsigned format = GBM_FORMAT_XRGB8888;
                typedef int (*eglGetConfigAttrib_t)(EGLDisplay, EGLConfig, EGLint, EGLint *);
                eglGetConfigAttrib_t get_attrib = (eglGetConfigAttrib_t)dlsym(RTLD_NEXT, "eglGetConfigAttrib");
                if (get_attrib) {
                    EGLint visual_id = 0;
                    if (get_attrib(dpy, config, 0x302E /* EGL_NATIVE_VISUAL_ID */, &visual_id) && visual_id != 0) {
                        fprintf(stderr, "=== SHIM: EGL_NATIVE_VISUAL_ID = 0x%x ===\n", visual_id);
                        format = (unsigned)visual_id;
                    } else {
                        fprintf(stderr, "=== SHIM: eglGetConfigAttrib(NATIVE_VISUAL_ID) failed/zero, using XRGB8888 ===\n");
                    }
                }

                if (surface_create) {
                    struct gbm_surface *surf = surface_create(gbm, width, height, format,
                                                               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
                    if (surf) {
                        fprintf(stderr, "=== SHIM: created gbm_surface %p (%ux%u, format=0x%x), using it instead ===\n", (void *)surf, width, height, format);
                        actual_win = (EGLNativeWindowType)surf;
                    } else {
                        fprintf(stderr, "=== SHIM: gbm_surface_create failed ===\n");
                    }
                }
            }
        }
    }

    EGLSurface result = real_fn(dpy, config, actual_win, attrib_list);
    fprintf(stderr, "=== SHIM: eglCreateWindowSurface returned %p ===\n", result);
    if (!result) {
        typedef EGLint (*eglGetError_t)(void);
        static eglGetError_t get_error = NULL;
        if (!get_error) get_error = (eglGetError_t)dlsym(RTLD_NEXT, "eglGetError");
        if (get_error) {
            fprintf(stderr, "=== SHIM: eglGetError() = 0x%x ===\n", get_error());
        }
    } else {
        typedef unsigned int (*eglSurfaceAttrib_t)(EGLDisplay, EGLSurface, EGLint, EGLint);
        eglSurfaceAttrib_t set_attrib = (eglSurfaceAttrib_t)dlsym(RTLD_NEXT, "eglSurfaceAttrib");
        if (set_attrib) {
            unsigned int ok = set_attrib(dpy, result, 0x3086 /* EGL_RENDER_BUFFER */, 0x3085 /* EGL_SINGLE_BUFFER */);
            fprintf(stderr, "=== SHIM: eglSurfaceAttrib(EGL_RENDER_BUFFER, EGL_SINGLE_BUFFER) -> %u ===\n", ok);
        }
    }
    return result;
}

typedef unsigned int EGLBoolean;
typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static int swap_count = 0;

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    static eglSwapBuffers_t real_fn = NULL;
    if (!real_fn) real_fn = (eglSwapBuffers_t)dlsym(RTLD_NEXT, "eglSwapBuffers");
    EGLBoolean result = real_fn(dpy, surface);
    swap_count++;
    if (swap_count <= 10 || swap_count % 60 == 0) {
        fprintf(stderr, "=== SHIM: eglSwapBuffers(dpy=%p, surface=%p) call #%d -> %u ===\n",
                dpy, surface, swap_count, result);
        if (!result) {
            typedef EGLint (*eglGetError_t)(void);
            static eglGetError_t get_error = NULL;
            if (!get_error) get_error = (eglGetError_t)dlsym(RTLD_NEXT, "eglGetError");
            if (get_error) {
                fprintf(stderr, "=== SHIM: eglSwapBuffers eglGetError() = 0x%x ===\n", get_error());
            }
        }
    }
    return result;
}

#include <sys/ioctl.h>
typedef int (*ioctl_t)(int, unsigned long, ...);

int ioctl(int fd, unsigned long request, ...) {
    static ioctl_t real_fn = NULL;
    if (!real_fn) real_fn = (ioctl_t)dlsym(RTLD_NEXT, "ioctl");
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    unsigned long grp = (request >> 8) & 0xff;
    if (grp == 0x46) { /* 'F' - fbdev ioctls */
        fprintf(stderr, "=== SHIM: ioctl(fd=%d, request=0x%lx) [fbdev] ===\n", fd, request);
    }
    return real_fn(fd, request, arg);
}
