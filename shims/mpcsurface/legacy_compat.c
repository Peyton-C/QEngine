/* Linked only into the build of mpcsurface for a guest glibc older than 2.33,
 * MPC 2.x on Buildroot (2.28 on 2.7.x, 2.32 on 2.11.x). Those libcs export
 * fstat only as __fxstat; fstat itself was a static wrapper in libc_nonshared.a,
 * which a firmware rootfs does not ship. This is that wrapper. It is hidden so
 * that it serves mpcsurface alone and is never interposed on MPC, which was
 * built against the same old glibc and calls __fxstat directly. */
#include <sys/stat.h>

#if !defined(__arm__)
#error "only armhf MPC 2.x has a pre-2.33 glibc; _STAT_VER below is the 32-bit ARM value"
#endif

extern int __fxstat(int ver, int fd, struct stat *buf);

__attribute__((visibility("hidden")))
int fstat(int fd, struct stat *buf) {
    return __fxstat(3 /* _STAT_VER_LINUX on 32-bit ARM */, fd, buf);
}
