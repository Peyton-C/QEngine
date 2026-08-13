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
 *
 * /proc/interrupts handling:
 * Engine hard-throws (uncaught std::runtime_error, aborts the process) if it
 * can't find a real-hardware IRQ line by name for six components QEMU's virt
 * machine doesn't emulate: dwc3, fe210000.sata, fea10000.dma-controller,
 * ff0c0000.dwmmc, ff0f0000.dwmmc, ttyS0. Immediately after finding each one
 * by name, Engine also writes CPU affinity to the matching
 * /proc/irq/<N>/smp_affinity, so whatever number we hand back has to be a
 * real, currently-live, writable IRQ too — a static pre-generated fake file
 * (the old approach, still present as a last-resort fallback in
 * fake-dt-rmz2/interrupts) goes stale the moment the QEMU device list
 * changes, since IRQ/MSI-vector numbers are assigned dynamically at boot
 * based on exactly which devices are present and in what order. Instead,
 * generate the fake content at runtime: read the real /proc/interrupts,
 * pick real IRQs that are (a) MSI-routed edge interrupts — the category
 * every virtio/USB-class device IRQ we've observed under this QEMU setup
 * falls into and reliably accepts affinity writes on (matches both the
 * plain "MSI" label this kernel uses and "ITS-MSI", seen on others — the
 * exact controller label varies by kernel build) — and (b) verified
 * writable by an actual no-op read-then-write-back probe (not just a
 * permission check; a GPIO-backed IRQ tried early in this project rejected
 * the write with EIO despite passing access(W_OK)), then relabel six of
 * them with the fake names Engine is looking for. Picked once per process
 * and cached, so repeated lookups within one boot stay consistent.
 *
 * That probe alone isn't sufficient, though — confirmed directly: an IRQ
 * that passes the probe at /proc/interrupts-read time can still reject
 * Engine's own affinity write moments later with the exact same EPERM,
 * because the underlying real device (whatever the fake name actually
 * landed on — an MSI vector genuinely owned by some virtio device) can
 * transition from freely-reaffinitizable to pinned once its own driver
 * finishes initializing, which happens on its own schedule, independent
 * of when Engine gets around to setting affinity for our fake names. A
 * second probe right before the real write wouldn't close this gap
 * either, just shrink it. Since the whole device is already fictional
 * (there's no real dwc3/dma-controller/etc. under QEMU at all), the
 * write() interceptor below fakes success for smp_affinity/
 * smp_affinity_list writes targeting the six real IRQ numbers we mapped,
 * the same way open()/fopen() already fake the device's existence —
 * consistent, and immune to the timing issue since it never depends on
 * whether the real kernel would have actually accepted the write.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <errno.h>
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

/* Real IRQ numbers our six fake names got mapped onto, populated once by
 * build_fake_interrupts() below and consulted by the write() interceptor
 * further down. Not lock-protected on the read side: written once, early,
 * before Engine ever gets to the point of setting any IRQ affinity, and
 * this is a development shim, not code that needs to survive a real race
 * auditor.
 *
 * Engine doesn't do the actual smp_affinity write itself — it shells out
 * ("sh -c 'echo ... > /proc/irq/N/smp_affinity'"). That child is a fresh
 * exec of /bin/sh, not a fork continuing Engine's own memory, so even
 * though it inherits LD_PRELOAD and loads this same .so, it gets its own
 * brand-new, empty copy of these globals — it never itself opens
 * /proc/interrupts, so it never populates them. Confirmed directly: a
 * bare `sh -c "echo 1 > /proc/irq/N/smp_affinity"` with LD_PRELOAD set
 * but no prior /proc/interrupts read in that process does nothing but
 * pass through to the real (failing) write.
 *
 * Fixed the same way LD_PRELOAD itself reaches that child: propagate the
 * mapping via an environment variable once computed, and fall back to
 * parsing it from the environment if this process's own array is empty. */
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
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ",", &saveptr); tok && (size_t)g_fake_mapped_irqs_count < NUM_FAKE_IRQS;
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
 * enough, some IRQ types (GPIO-backed, per-CPU PPIs like arm-pmu) have
 * writable permission bits but return EIO from the driver on an actual
 * write. */
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
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    return w == n;
}

typedef struct {
    long irq;
    char *line; /* full real /proc/interrupts line for this IRQ, no newline */
} irq_candidate_t;

/* Reads the real /proc/interrupts, returns a malloc'd fake-content string
 * with the six FAKE_IRQ_NAMES relabeled onto real, verified-writable
 * ITS-MSI/Edge IRQs, or NULL if no usable candidates were found (caller
 * falls back to the static file). */
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
            size_t need = out_len + (size_t)len + 1;
            if (need > out_cap) {
                out_cap = need * 2;
                out = realloc(out, out_cap);
            }
            out_len += (size_t)snprintf(out + out_len, out_cap - out_len, "%s\n", line);
            continue;
        }

        /* Match "MSI" rather than "ITS-MSI" specifically — this also
         * matches "ITS-MSI" (contains "MSI" as a substring), so it still
         * covers the Ubuntu cloud kernel's labeling, but this Debian
         * trixie kernel's controller column says plain "MSI" with no
         * "ITS-" prefix at all, which the original ITS-MSI-only match
         * never matched — meaning ncand was always 0 here, and every
         * "successful" run on this kernel, including earlier interactive
         * tests, was silently taking the static fallback file the whole
         * time, never actually exercising this path. */
        if (!strstr(line, "MSI") || !strstr(line, "Edge")) continue;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        long irq = strtol(line, NULL, 10);
        if (irq <= 0) continue;
        if (!irq_affinity_writable(irq)) continue;

        if (ncand == cand_cap) {
            cand_cap = cand_cap ? cand_cap * 2 : 8;
            cands = realloc(cands, cand_cap * sizeof(*cands));
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
        char *last_space = strrchr(tmp, ' ');
        const char *prefix = tmp;
        if (last_space) *last_space = '\0';

        size_t need = out_len + strlen(prefix) + strlen(FAKE_IRQ_NAMES[i]) + 8;
        if (need > out_cap) {
            out_cap = need * 2;
            out = realloc(out, out_cap);
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
 * fake-dt-rmz2/interrupts file). Content is generated once per process and
 * cached — every caller gets its own independent fd/position over the same
 * cached text, matching normal open() semantics for multiple readers. */
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

/* Fakes success for smp_affinity/smp_affinity_list writes targeting the
 * real IRQ numbers our six fake names got mapped onto — see the big
 * comment at the top of this file for why a probe-then-use approach
 * alone isn't reliable here. Everything else passes straight through. */
ssize_t write(int fd, const void *buf, size_t count) {
    /* !g_fake_mapped_irqs_env_checked, not g_fake_mapped_irqs_count > 0 —
     * a freshly-exec'd child (e.g. Engine's "sh -c echo ... > ...") starts
     * with count still at 0 and needs at least one call through here to
     * get the lazy env-var load (inside is_fake_mapped_irq()) a chance to
     * run before there's anything to check count against. */
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

/* Devicetree-access logging. remap() below only special-cases the handful
 * of /sys/firmware/devicetree/base/... paths we already know Engine reads
 * (product-code, serial-number, rotation) — anything else under that tree
 * (or its /proc/device-tree/ alias) passes straight through unnoticed to
 * QEMU's synthesized virt devicetree, which has none of the real
 * inmusic,-prefixed or az04-* nodes. If Engine's audio-device gate reads some
 * devicetree property we haven't identified yet — e.g. something naming
 * the onboard az04-codec/simple-audio-card — that read is currently
 * silent. This logs every open/fopen attempt (success or failure) under
 * either prefix so a real boot+track-load can be grepped afterward for
 * exactly what Engine asked for.
 *
 * Off unless DTSHIM_DT_LOG is set. It served its purpose — two full passes
 * showed only product-code, serial-number and one stale RK3288-era rotation
 * probe, ruling the devicetree out of the audio investigation entirely — and
 * it is not free to leave on: every matching access takes a mutex and does a
 * separate fopen/fprintf/fclose, and serial-number alone is re-read dozens of
 * times per session. */
#define DT_ACCESS_LOG "/root/dtshim-dt-access.log"
static pthread_mutex_t dt_log_lock = PTHREAD_MUTEX_INITIALIZER;

/* Resolved once: getenv() on every devicetree read would itself be overhead
 * in a path that exists only for diagnostics. */
static int dt_log_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("DTSHIM_DT_LOG");
        enabled = (v && *v) ? 1 : 0;
    }
    return enabled;
}

static int is_dt_path(const char *path) {
    if (!path || !dt_log_enabled()) return 0;
    return strncmp(path, "/sys/firmware/devicetree/base/", 30) == 0 ||
           strncmp(path, "/proc/device-tree/", 18) == 0;
}

/* fn: calling wrapper's name, orig_path: what Engine actually asked to
 * open, remapped_path: what we opened after remap() (same pointer as
 * orig_path if unmapped), ok/err: outcome to log. Only ever called with
 * DT-tree paths, so no need to re-check is_dt_path() at call sites past
 * the initial gate. Saves/restores errno around its own real_fopen/fclose
 * calls so it never perturbs what the caller (Engine) observes. */
static void log_dt_access(const char *fn, const char *orig_path,
                           const char *remapped_path, int ok, int err) {
    int saved_errno = errno;
    pthread_mutex_lock(&dt_log_lock);
    FILE *lf = get_real_fopen()(DT_ACCESS_LOG, "a");
    if (lf) {
        if (remapped_path && strcmp(orig_path, remapped_path) != 0) {
            fprintf(lf, "[pid %d] %s(\"%s\") -> remapped \"%s\": %s\n",
                    getpid(), fn, orig_path, remapped_path,
                    ok ? "ok" : strerror(err));
        } else {
            fprintf(lf, "[pid %d] %s(\"%s\"): %s\n",
                    getpid(), fn, orig_path, ok ? "ok" : strerror(err));
        }
        fclose(lf);
    }
    pthread_mutex_unlock(&dt_log_lock);
    errno = saved_errno;
}

static const char *remap(const char *path) {
    if (!path) return NULL;
    if (strcmp(path, "/sys/firmware/devicetree/base/inmusic,product-code") == 0)
        return "/root/fake-dt/inmusic,product-code";
    if (strcmp(path, "/sys/firmware/devicetree/base/serial-number") == 0)
        return "/root/fake-dt/serial-number";
    if (strcmp(path, "/dev/mem") == 0)
        return "/root/fake-dev-mem";
    if (strcmp(path, "/sys/firmware/devicetree/base/dsi@fde20000/panel@0/rotation") == 0 || strcmp(path, "/sys/firmware/devicetree/base/dsi@fde30000/panel@0/rotation") == 0 || strcmp(path, "/sys/firmware/devicetree/base/mipi@ff960000/panel@0/rotation") == 0 || strcmp(path, "/sys/firmware/devicetree/base/edp-panel/rotation") == 0)
        return "/root/fake-dt/rotation";
    /* Static fallback only — see get_fake_interrupts_fd() above, which is
     * tried first and covers the normal case dynamically. */
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
    const char *mapped = remap(path);
    int fd = get_real_open()(mapped, flags, mode);
    if (is_dt_path(path)) log_dt_access("open", path, mapped, fd >= 0, errno);
    return fd;
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
    const char *mapped = remap(path);
    int fd = real_open64(mapped, flags, mode);
    if (is_dt_path(path)) log_dt_access("open64", path, mapped, fd >= 0, errno);
    return fd;
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
    const char *mapped = remap(path);
    FILE *f = get_real_fopen()(mapped, mode);
    if (is_dt_path(path)) log_dt_access("fopen", path, mapped, f != NULL, errno);
    return f;
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
    const char *mapped = remap(path);
    FILE *f = real_fopen64(mapped, mode);
    if (is_dt_path(path)) log_dt_access("fopen64", path, mapped, f != NULL, errno);
    return f;
}