#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ucontext.h>
#include <fcntl.h>
#include <string.h>
#include <execinfo.h>

#define MAX_REGIONS 512
struct region {
    unsigned long start, end;
    char path[256];
};
static struct region regions[MAX_REGIONS];
static int nregions = 0;

static void load_maps(void) {
    nregions = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && nregions < MAX_REGIONS) {
        unsigned long start, end;
        char perms[8];
        char path[256];
        path[0] = '\0';
        int matched = sscanf(line, "%lx-%lx %7s %*x %*x:%*x %*d %255[^\n]", &start, &end, perms, path);
        if (matched >= 3 && perms[2] == 'x') {
            regions[nregions].start = start;
            regions[nregions].end = end;
            strncpy(regions[nregions].path, path, sizeof(regions[nregions].path) - 1);
            nregions++;
        }
    }
    fclose(f);
}

static const struct region *find_region(unsigned long addr) {
    for (int i = 0; i < nregions; i++) {
        if (addr >= regions[i].start && addr < regions[i].end) return &regions[i];
    }
    return NULL;
}

static void handler(int sig, siginfo_t *si, void *ucontext_v) {
    ucontext_t *uc = (ucontext_t *)ucontext_v;
    char buf[512];
    int n;
    unsigned long pc = 0, lr = 0, sp = 0;
#if defined(__arm__)
    pc = uc->uc_mcontext.arm_pc;
    lr = uc->uc_mcontext.arm_lr;
    sp = uc->uc_mcontext.arm_sp;
    n = snprintf(buf, sizeof(buf),
        "\n=== CRASHHANDLER: signal %d, fault addr=%p, PC=0x%08lx, LR=0x%08lx, SP=0x%08lx, pid=%d ===\n",
        sig, si->si_addr, pc, lr, sp, getpid());
#else
    n = snprintf(buf, sizeof(buf),
        "\n=== CRASHHANDLER: signal %d, fault addr=%p, pid=%d ===\n",
        sig, si->si_addr, getpid());
#endif
    write(2, buf, n);

    load_maps();

    const struct region *r = find_region(pc);
    n = snprintf(buf, sizeof(buf), "PC region: %s + 0x%lx\n", r ? r->path : "?", r ? pc - r->start : 0);
    write(2, buf, n);
    r = find_region(lr);
    n = snprintf(buf, sizeof(buf), "LR region: %s + 0x%lx\n", r ? r->path : "?", r ? lr - r->start : 0);
    write(2, buf, n);

    write(2, "=== stack scan (candidate return addrs) ===\n", 45);
    unsigned long *stack = (unsigned long *)sp;
    for (int i = 0; i < 2048; i++) {
        unsigned long val = stack[i];
        const struct region *reg = find_region(val);
        if (reg) {
            n = snprintf(buf, sizeof(buf), "  [sp+0x%04x] 0x%08lx  %s + 0x%lx\n",
                i * 4, val, reg->path, val - reg->start);
            write(2, buf, n);
        }
    }

    char mapspath[64];
    snprintf(mapspath, sizeof(mapspath), "/proc/self/maps");
    int fd = open(mapspath, O_RDONLY);
    if (fd >= 0) {
        char mbuf[4096];
        ssize_t rr;
        write(2, "=== /proc/self/maps ===\n", 25);
        while ((rr = read(fd, mbuf, sizeof(mbuf))) > 0) {
            write(2, mbuf, rr);
        }
        close(fd);
    }
    _exit(200);
}

__attribute__((constructor))
static void install_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}