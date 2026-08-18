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
 * correct against Engine 5.0.4's shared/universal /usr/Engine tree).
 *
 * /proc/interrupts handling:
 * The IRQ-affinity check this file previously only guarded against
 * "defensively" is real on RK3288 too, and it is fatal. Confirmed on a
 * JP13 (SC6000) guest, on this branch and on main alike:
 *
 *     terminate called after throwing an instance of 'std::runtime_error'
 *       what():  No IRQ matching 'ttyS0' found in /proc/interrupts
 *
 * followed by SIGABRT, systemd restart, and a crash loop that never gets
 * far enough to bring up a display — the guest sits at a black screen
 * indefinitely. The static placeholder the armv7 builder writes only ever
 * contained arch_timer and uart-pl011 lines, so the lookup could never
 * have succeeded; this has never worked on armv7, rather than having
 * regressed.
 *
 * The fix is the mechanism already proven on RK3588, ported here. See the
 * long commentary in dtshim_rmz2.c for the full reasoning; in brief:
 * Engine looks up several real-hardware components by name and, having
 * found each, writes CPU affinity to the matching /proc/irq/<N>/smp_affinity.
 * So the IRQ number handed back has to be real, live and writable, which a
 * static file cannot guarantee — IRQ numbers are assigned at boot from
 * whichever devices happen to be present. Instead the real /proc/interrupts
 * is read at runtime, usable IRQs are picked from it, and the names Engine
 * wants are relabeled onto them.
 *
 * ONE DELIBERATE DIVERGENCE FROM THE RK3588 VERSION, and it is the whole
 * reason this could not simply be compiled from that file: RMZ2 selects
 * candidates with `strstr(line, "MSI") && strstr(line, "Edge")`. The 32-bit
 * `virt` machine has no usable PCI (see scripts/qemu/arch_devices.sh), so
 * its virtio devices are virtio-mmio and every IRQ is GIC-routed — there is
 * not one MSI line in the guest's /proc/interrupts:
 *
 *      31:  ...  9030000.pl061   3 Edge
 *      32:  ...  GIC-0  74 Edge      virtio1
 *      36:  ...  GIC-0  73 Edge      virtio0
 *
 * Requiring MSI here would match nothing, produce zero candidates, and fall
 * silently back to the same static file that is already broken — the exact
 * failure mode dtshim_rmz2.c documents having hit when it required "ITS-MSI"
 * on a kernel that labels them plain "MSI". So this matches on Edge alone
 * and leans on the write-back probe to reject what is not really writable,
 * which is what actually establishes usability in either case. The GPIO
 * IRQ visible above (9030000.pl061) is precisely the kind that advertises
 * writable permissions and then returns EIO, and the probe is what catches
 * it.
 *
 * The FAKE_IRQ_NAMES list is carried over whole from RK3588. Only ttyS0 is
 * confirmed to matter here — it is the name Engine threw on — but the list
 * costs nothing: a name this device never looks up is just an unread line.
 * Engine surfaces these one at a time, each as another hard throw, so if
 * RK3288 wants a name that is not here it will say so in the same way, in
 * the journal, and it can be appended.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

typedef int (*open_t)(const char *, int, ...);
typedef int (*open64_t)(const char *, int, ...);
typedef FILE *(*fopen_t)(const char *, const char *);
typedef FILE *(*fopen64_t)(const char *, const char *);
typedef ssize_t (*write_t)(int, const void *, size_t);

/* Shared real-libc handles, resolved once and reused both by the wrapper
 * functions below and by the internal /proc/interrupts generation/probing
 * code, so the latter never risks going back through our own wrappers. */
static open_t real_open = NULL;
static fopen_t real_fopen = NULL;
static write_t real_write = NULL;

static write_t get_real_write(void) {
    if (!real_write) real_write = (write_t)dlsym(RTLD_NEXT, "write");
    return real_write;
}

static open_t get_real_open(void) {
    if (!real_open) real_open = (open_t)dlsym(RTLD_NEXT, "open");
    return real_open;
}

static fopen_t get_real_fopen(void) {
    if (!real_fopen) real_fopen = (fopen_t)dlsym(RTLD_NEXT, "fopen");
    return real_fopen;
}

static const char *FAKE_IRQ_NAMES[] = {
    "dwc3",
    "fe210000.sata",
    "fea10000.dma-controller",
    "ff0c0000.dwmmc",
    "ff0f0000.dwmmc",
    "ttyS0",
};
#define NUM_FAKE_IRQS (sizeof(FAKE_IRQ_NAMES) / sizeof(FAKE_IRQ_NAMES[0]))

/* Real IRQ numbers our fake names got mapped onto, populated once by
 * build_fake_interrupts() and consulted by the write() interceptor.
 *
 * Engine doesn't do the smp_affinity write itself — it shells out
 * ("sh -c 'echo ... > /proc/irq/N/smp_affinity'"). That child is a fresh
 * exec, so although it inherits LD_PRELOAD and loads this same .so, it gets
 * its own empty copy of these globals and never reads /proc/interrupts
 * itself. Propagated through the environment for exactly that reason, the
 * same way LD_PRELOAD reaches the child in the first place. */
#define FAKE_IRQ_ENV_VAR "DTSHIM_FAKE_IRQS"

static long g_fake_mapped_irqs[NUM_FAKE_IRQS];
static int g_fake_mapped_irqs_count = 0;
static int g_fake_mapped_irqs_env_checked = 0;

static void publish_fake_irqs_to_env(void) {
    char buf[256];
    size_t len = 0;
    for (int i = 0; i < g_fake_mapped_irqs_count && len < sizeof(buf) - 1; i++) {
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s%ld",
                                 i ? "," : "", g_fake_mapped_irqs[i]);
    }
    setenv(FAKE_IRQ_ENV_VAR, buf, 1);
}

static void load_fake_irqs_from_env_if_needed(void) {
    if (g_fake_mapped_irqs_count > 0 || g_fake_mapped_irqs_env_checked) return;
    g_fake_mapped_irqs_env_checked = 1;

    const char *env = getenv(FAKE_IRQ_ENV_VAR);
    if (!env || !*env) return;

    char *copy = strdup(env);
    if (!copy) return;
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ",", &saveptr);
         tok && (size_t)g_fake_mapped_irqs_count < NUM_FAKE_IRQS;
         tok = strtok_r(NULL, ",", &saveptr)) {
        g_fake_mapped_irqs[g_fake_mapped_irqs_count++] = strtol(tok, NULL, 10);
    }
    free(copy);
}

static int is_fake_mapped_irq(long irq) {
    load_fake_irqs_from_env_if_needed();
    for (int i = 0; i < g_fake_mapped_irqs_count; i++) {
        if (g_fake_mapped_irqs[i] == irq) return 1;
    }
    return 0;
}

/* Real, no-op read-then-write-back probe of whether the kernel actually
 * accepts an smp_affinity write for this IRQ — access(W_OK) alone isn't
 * enough, some IRQ types (GPIO-backed, per-CPU PPIs) have writable
 * permission bits but return EIO from the driver on an actual write. */
static int irq_affinity_writable(long irq) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/irq/%ld/smp_affinity", irq);

    int fd = get_real_open()(path, O_RDWR);
    if (fd < 0) return 0;

    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
        close(fd);
        return 0;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return 0;
    }
    ssize_t w = get_real_write()(fd, buf, (size_t)n);
    close(fd);
    return w == n;
}

typedef struct {
    long irq;
    char *line; /* full real /proc/interrupts line for this IRQ, no newline */
} irq_candidate_t;

/* Reads the real /proc/interrupts and returns malloc'd fake content with
 * FAKE_IRQ_NAMES relabeled onto real, verified-writable Edge IRQs, or NULL
 * if no usable candidates were found (caller falls back to the static
 * file). */
static char *build_fake_interrupts(void) {
    FILE *f = get_real_fopen()("/proc/interrupts", "r");
    if (!f) return NULL;

    char *out = NULL;
    size_t out_len = 0, out_cap = 0;
    irq_candidate_t *cands = NULL;
    size_t ncand = 0, cand_cap = 0;

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t len;
    int first = 1;

    while ((len = getline(&line, &line_cap, f)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (first) {
            /* header line ("           CPU0  CPU1 ...") — keep verbatim */
            first = 0;
            size_t need = out_len + (size_t)len + 2;
            if (need > out_cap) {
                out_cap = need * 2;
                out = realloc(out, out_cap);
                if (!out) { free(line); fclose(f); return NULL; }
            }
            out_len += (size_t)snprintf(out + out_len, out_cap - out_len, "%s\n", line);
            continue;
        }

        /* Edge only, no MSI requirement — see the divergence note at the top
         * of this file. Everything usable on the 32-bit virt machine is a
         * GIC-routed virtio-mmio Edge IRQ. */
        if (!strstr(line, "Edge")) continue;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        long irq = strtol(line, NULL, 10);
        if (irq <= 0) continue;
        if (!irq_affinity_writable(irq)) continue;

        if (ncand == cand_cap) {
            cand_cap = cand_cap ? cand_cap * 2 : 8;
            cands = realloc(cands, cand_cap * sizeof(*cands));
            if (!cands) break;
        }
        cands[ncand].irq = irq;
        cands[ncand].line = strdup(line);
        ncand++;
    }
    free(line);
    fclose(f);

    if (ncand == 0) {
        free(out);
        free(cands);
        return NULL;
    }

    for (size_t i = 0; i < NUM_FAKE_IRQS; i++) {
        long real_irq = cands[i % ncand].irq;
        const char *real_line = cands[i % ncand].line;
        char *tmp = strdup(real_line);
        if (!tmp) break;
        char *last_space = strrchr(tmp, ' ');
        const char *prefix = tmp;
        if (last_space) *last_space = '\0';

        size_t need = out_len + strlen(prefix) + strlen(FAKE_IRQ_NAMES[i]) + 8;
        if (need > out_cap) {
            out_cap = need * 2;
            out = realloc(out, out_cap);
            if (!out) { free(tmp); break; }
        }
        out_len += (size_t)snprintf(out + out_len, out_cap - out_len, "%s %s\n",
                                     prefix, FAKE_IRQ_NAMES[i]);
        free(tmp);

        g_fake_mapped_irqs[g_fake_mapped_irqs_count++] = real_irq;
    }

    for (size_t i = 0; i < ncand; i++) free(cands[i].line);
    free(cands);
    publish_fake_irqs_to_env();
    return out;
}

static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;
static char *fake_interrupts_content = NULL;
static int fake_interrupts_generation_failed = 0;

/* Returns an fd (from an anonymous memfd, so nothing touches the real
 * filesystem) with freshly-seeked fake /proc/interrupts content, or -1 if
 * generation isn't possible this run (caller falls back to the static
 * file). Generated once per process and cached — every caller gets its own
 * fd/position over the same text, matching open() semantics for multiple
 * readers. */
static int get_fake_interrupts_fd(void) {
    pthread_mutex_lock(&gen_lock);
    if (!fake_interrupts_content && !fake_interrupts_generation_failed) {
        fake_interrupts_content = build_fake_interrupts();
        if (!fake_interrupts_content) fake_interrupts_generation_failed = 1;
    }
    char *content = fake_interrupts_content;
    pthread_mutex_unlock(&gen_lock);

    if (!content) return -1;

    int fd = memfd_create("fake-proc-interrupts", MFD_CLOEXEC);
    if (fd < 0) return -1;

    size_t len = strlen(content);
    if (get_real_write()(fd, content, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* If path is "/proc/irq/<N>/smp_affinity" or ".../smp_affinity_list",
 * extracts N. Returns 1 on match, 0 otherwise. */
static int parse_irq_affinity_path(const char *path, long *out_irq) {
    long irq;
    int consumed = 0;
    if (sscanf(path, "/proc/irq/%ld/smp_affinity_list%n", &irq, &consumed) == 1 &&
        path[consumed] == '\0') {
        *out_irq = irq;
        return 1;
    }
    if (sscanf(path, "/proc/irq/%ld/smp_affinity%n", &irq, &consumed) == 1 &&
        path[consumed] == '\0') {
        *out_irq = irq;
        return 1;
    }
    return 0;
}

/* Fakes success for smp_affinity writes targeting the real IRQ numbers our
 * fake names got mapped onto. A probe-then-use approach alone isn't
 * reliable: an IRQ that passes the probe can still reject Engine's write
 * moments later, because the real device underneath can become pinned once
 * its own driver finishes initialising, on its own schedule. Since the
 * device is fictional anyway, faking the write is consistent with already
 * faking its existence — and immune to that timing. */
ssize_t write(int fd, const void *buf, size_t count) {
    /* !g_fake_mapped_irqs_env_checked, not count > 0 — a freshly-exec'd
     * child starts with count at 0 and needs one call through here to give
     * the lazy env load (inside is_fake_mapped_irq) a chance to run. */
    if (!g_fake_mapped_irqs_env_checked || g_fake_mapped_irqs_count > 0) {
        char linkpath[64], target[256];
        snprintf(linkpath, sizeof(linkpath), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(linkpath, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            long irq;
            if (parse_irq_affinity_path(target, &irq) && is_fake_mapped_irq(irq)) {
                return (ssize_t)count;
            }
        }
    }
    return get_real_write()(fd, buf, count);
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
    /* Static fallback only — get_fake_interrupts_fd() is tried first and
     * covers the normal case dynamically. */
    if (strcmp(path, "/proc/interrupts") == 0)
        return "/root/fake-dt/interrupts";
    return path;
}

int open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);

    if (path && strcmp(path, "/proc/interrupts") == 0) {
        int fd = get_fake_interrupts_fd();
        if (fd >= 0) return fd;
    }
    return get_real_open()(remap(path), flags, mode);
}

int open64(const char *path, int flags, ...) {
    static open64_t real_open64 = NULL;
    if (!real_open64) real_open64 = (open64_t)dlsym(RTLD_NEXT, "open64");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);

    if (path && strcmp(path, "/proc/interrupts") == 0) {
        int fd = get_fake_interrupts_fd();
        if (fd >= 0) return fd;
    }
    return real_open64(remap(path), flags, mode);
}

FILE *fopen(const char *path, const char *mode) {
    if (path && strcmp(path, "/proc/interrupts") == 0) {
        int fd = get_fake_interrupts_fd();
        if (fd >= 0) {
            FILE *f = fdopen(fd, mode);
            if (f) return f;
            close(fd);
        }
    }
    return get_real_fopen()(remap(path), mode);
}

FILE *fopen64(const char *path, const char *mode) {
    static fopen64_t real_fopen64 = NULL;
    if (!real_fopen64) real_fopen64 = (fopen64_t)dlsym(RTLD_NEXT, "fopen64");

    if (path && strcmp(path, "/proc/interrupts") == 0) {
        int fd = get_fake_interrupts_fd();
        if (fd >= 0) {
            FILE *f = fdopen(fd, mode);
            if (f) return f;
            close(fd);
        }
    }
    return real_fopen64(remap(path), mode);
}
