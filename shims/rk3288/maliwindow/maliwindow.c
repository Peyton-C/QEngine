/* Looper X 1.0.2 links Qt and eglfs_mali statically, so an eglfs_kms plugin
 * cannot replace its Mali integration. Keep the app's fbdev_window ABI and
 * render into a Mesa pbuffer, then copy each frame to the kernel framebuffer.
 * MALIWINDOW_ROTATE=90 undoes HG08's fixed portrait-panel rotation. This is
 * software rendering; it does not provide Mali acceleration or host audio.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
static EGLSurface window_surface;
static unsigned width, height;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static unsigned char *fb, *pixels;

EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) {
    EGLBoolean (*real)(EGLDisplay, EGLSurface) = dlsym(RTLD_NEXT, "eglDestroySurface");
    if (s == window_surface) {
        if (fb)
            munmap(fb, fi.smem_len);
        free(pixels);
        fb = pixels = NULL;
        window_surface = EGL_NO_SURFACE;
    }
    return real(d, s);
}
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attrs, EGLConfig *cfg, EGLint n,
                           EGLint *count) {
    EGLBoolean (*real)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) =
        dlsym(RTLD_NEXT, "eglChooseConfig");
    EGLint a[129];
    unsigned i = 0;
    if (attrs)
        for (; i < 124 && attrs[i] != EGL_NONE; i += 2) {
            a[i] = attrs[i];
            a[i + 1] = attrs[i + 1];
            if (a[i] == EGL_SURFACE_TYPE)
                a[i + 1] = EGL_PBUFFER_BIT;
        }
    if (attrs && attrs[i] != EGL_NONE)
        return EGL_FALSE;
    a[i] = EGL_NONE;
    return real(d, a, cfg, n, count);
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType win,
                                  const EGLint *attrs) {
    (void)attrs;
    // Qt's Mali integration allocates fbdev_window { unsigned short width,height; }.
    unsigned short *size = (void *)win;
    width = size[0];
    height = size[1];
    if (!width || !height)
        return EGL_NO_SURFACE;
    EGLint a[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    window_surface = eglCreatePbufferSurface(d, c, a);
    if (window_surface == EGL_NO_SURFACE)
        return window_surface;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd >= 0 && !ioctl(fd, FBIOGET_VSCREENINFO, &vi) && !ioctl(fd, FBIOGET_FSCREENINFO, &fi)) {
        fb = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (fb == MAP_FAILED)
            fb = NULL;
        if (fb && (vi.bits_per_pixel != 32 || fi.line_length < (size_t)vi.xres * 4 ||
                   (size_t)fi.line_length * vi.yres > fi.smem_len)) {
            munmap(fb, fi.smem_len);
            fb = NULL;
        }
    }
    if (fd >= 0)
        close(fd);
    pixels = malloc((size_t)width * height * 4);
    fprintf(stderr, "maliwindow: %ux%u pbuffer %p, fb %ux%u/%u mapped %p\n", width, height,
            window_surface, vi.xres, vi.yres, vi.bits_per_pixel, fb);
    return window_surface;
}
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    EGLBoolean (*real)(EGLDisplay, EGLSurface) = dlsym(RTLD_NEXT, "eglSwapBuffers");
    if (s == window_surface && fb && pixels && vi.bits_per_pixel == 32) {
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        int rotate = getenv("MALIWINDOW_ROTATE") && atoi(getenv("MALIWINDOW_ROTATE")) == 90;
        unsigned rw = rotate ? height : width, rh = rotate ? width : height;
        unsigned w = rw < vi.xres ? rw : vi.xres, h = rh < vi.yres ? rh : vi.yres;
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) {
                unsigned char *p =
                    pixels + (rotate ? (x * width + y) : ((height - 1 - y) * width + x)) * 4;
                uint32_t color = ((uint32_t)p[0] << vi.red.offset) |
                                 ((uint32_t)p[1] << vi.green.offset) |
                                 ((uint32_t)p[2] << vi.blue.offset);
                // virtio's XRGB framebuffer has no alpha channel, with offset zero.
                // Writing alpha there would force the blue channel to 255.
                if (vi.transp.length)
                    color |= (uint32_t)p[3] << vi.transp.offset;
                *(uint32_t *)(fb + y * fi.line_length + x * 4) = color;
            }
    }
    return real(d, s);
}
