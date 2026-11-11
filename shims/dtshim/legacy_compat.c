/* Additional entry points for the glibc 2.25 build of dtshim, linked only into
 * the JP07-era artifact. SC5000 1.2.0 probes physical register 0xff7e0050 with
 * the 32-bit mmap API. Its signed offset is not a usable backing-file offset;
 * serve the fictitious registers as anonymous zero-filled pages instead. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* memfd_create entered glibc after 2.25, but the shared kernel supports it. */
int memfd_create(const char *name, unsigned int flags) {
    return syscall(SYS_memfd_create, name, flags);
}

static int fake_mem_fd(int fd) {
    char path[64], target[512];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) return 0;
    target[n] = 0;
    return strcmp(target, "/root/fake-dev-mem") == 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    if (fake_mem_fd(fd))
        return (void *)syscall(SYS_mmap2, addr, len, prot,
                              (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *(*real_mmap)(void *, size_t, int, int, int, off_t) = dlsym(RTLD_NEXT, "mmap");
    return real_mmap(addr, len, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off64_t offset) {
    if (fake_mem_fd(fd))
        return (void *)syscall(SYS_mmap2, addr, len, prot,
                              (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *(*real_mmap)(void *, size_t, int, int, int, off64_t) = dlsym(RTLD_NEXT, "mmap64");
    return real_mmap(addr, len, prot, flags, fd, offset);
}

/* Fortified calls otherwise bypass dtshim's public open wrappers. */
int __open_2(const char *path, int flags) { return open(path, flags, 0); }
int __open64_2(const char *path, int flags) { return open64(path, flags, 0); }
