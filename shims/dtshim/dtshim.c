/* Serves a fake devicetree, and a usable /proc/interrupts, to an emulated Engine.
 *
 * One source, two SoCs. Everything that matters here -- the open/openat/fopen and
 * write interposition, the runtime /proc/interrupts generation and its IRQ
 * affinity probing, the /dev/mem neutering -- is the same on both, which is why
 * this was two files that had diverged rather than two different shims. Define
 * SOC_RK3588 or SOC_RK3288 to pick which devicetree properties it serves; that
 * choice is the whole of the difference, and it lives in DT_REMAPS below.
 *
 * How the two devicetrees differ, for the record:
 *   - product-code is "RMZ2" on RK3588; JC11/JP11/JP14 and relatives on RK3288
 *   - no inmusic,az01-pcb-rev property exists in RMZ2's devicetree
 *   - the panel/rotation node lives at dsi@... or edp-panel on RK3588, and at
 *     mipi@ff960000 on RK3288
 *   - no chosen/inmusic,internal-sd-fitted property on RK3588
 *
 * Two things the RK3288 original carried that neither build wants any more:
 *   - the Qt5-ABI stub symbols are gone: this build is
 *     Qt 6.7.2, a different ABI, and that missing-symbol issue was never
 *     observed here
 *   - the eglCreateWindowSurface/eglSurfaceAttrib
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
#include <limits.h>
#include <dirent.h>
#include <stddef.h>
#include <sys/stat.h>

typedef int (*open_t)(const char *, int, ...);
typedef int (*open64_t)(const char *, int, ...);
typedef FILE *(*fopen_t)(const char *, const char *);
typedef FILE *(*fopen64_t)(const char *, const char *);
typedef ssize_t (*write_t)(int, const void *, size_t);
typedef int (*access_t)(const char *, int);
typedef int (*faccessat_t)(int, const char *, int, int);
typedef char *(*realpath_chk_t)(const char *, char *, size_t);
typedef DIR *(*opendir_t)(const char *);
typedef struct dirent64 *(*readdir64_t)(DIR *);
typedef int (*openat_t)(int, const char *, int, ...);
typedef int (*statx_t)(int, const char *, int, unsigned int, struct statx *);
typedef ssize_t (*readlink_t)(const char *, char *, size_t);

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
    int msi;    /* an MSI edge interrupt: the preferred kind, see below */
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
         * time, never actually exercising this path.
         *
         * MSI edge interrupts are a preference, not a requirement. They are
         * what a QEMU guest has plenty of, but a machine with no PCI devices
         * has none: a Raspberry Pi 5 lists only GICv2 and rp1_irq_chip lines,
         * so requiring MSI left ncand at 0 there and handed Engine the static
         * fallback, whose IRQ numbers do not exist on that machine -- its
         * "echo N > /proc/irq/16/smp_affinity_list" then failed at open, which
         * the write() interceptor never sees, and Engine aborted. Any IRQ that
         * survives the write-back probe will do, so the others are kept as a
         * second choice and used only when there is no MSI line at all. */
        int msi = strstr(line, "MSI") && strstr(line, "Edge");

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
        cands[ncand].msi = msi;
        ncand++;
    }
    free(line);
    fclose(f);

    /* Keep only the MSI candidates where there are any, so a QEMU guest picks
     * exactly what it always did. */
    size_t nmsi = 0;
    for (size_t i = 0; i < ncand; i++) nmsi += cands[i].msi ? 1 : 0;
    if (nmsi > 0 && nmsi < ncand) {
        size_t kept = 0;
        for (size_t i = 0; i < ncand; i++) {
            if (cands[i].msi) cands[kept++] = cands[i];
            else free(cands[i].line);
        }
        ncand = kept;
    }

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

/* The fixed part of this SoC's devicetree, served straight from here.
 *
 * These describe the hardware the guest is pretending to be, so they belong with
 * the DT_REMAPS table below that decides they are served at all, selected by the
 * same -DSOC_* flag. They used to be printf lines in the rootfs builders and one
 * committed fixture file, in two different styles.
 *
 * Identity is the exception and stays a written file: inmusic,product-code and
 * serial-number are per-instance, Engine shows the serial in Settings as
 * DeviceSerialNumber, and midisurface opens the product-code file directly to
 * decide which device to answer Engine's inquiry as. rootfs_steps/write_fake_dt.sh
 * writes those two.
 *
 * FALLBACK_INTERRUPTS is a last resort, not the normal path: build_fake_interrupts()
 * below derives the real thing at runtime, and this is only reached when that finds
 * no usable IRQ at all. It is plausible synthetic data captured from hardware. */

/* Raw big-endian <u32> devicetree cells, not text -- these carry NULs, so every
 * user passes an explicit length rather than relying on strlen. */
static const char DT_U32_ZERO[4] = {0, 0, 0, 0};

#if defined(SOC_RK3588)
static const char FALLBACK_INTERRUPTS[] =
    "           CPU0       CPU1       CPU2       CPU3       CPU4       CPU5       CPU6       CPU7\n"
    " 11:      19583      17716      20155      23272      18371      22086      23997      18307 GICv3  27 Level     arch_timer\n"
    " 13:          9          0          0          0          0          0          0          0 GICv3  33 Level     uart-pl011\n"
    " 16:        788          0        171          0          0          0          0          0   ITS-MSI 81920 Edge      dwc3\n"
    " 17:       3124          0          0          0          0          0          0         16   ITS-MSI 81921 Edge      fe210000.sata\n"
    " 18:          0          0          0          0          0          0          0          0   ITS-MSI 81922 Edge      fea10000.dma-controller\n"
    " 19:         30          0          0          0          0          0          0          0   ITS-MSI 81923 Edge      ff0c0000.dwmmc\n"
    " 20:         93          0          0          0          0          0          0          0   ITS-MSI 81924 Edge      ff0f0000.dwmmc\n"
    " 23:          0          0          0          0          0          0          0          0   ITS-MSI 49152 Edge      ttyS0\n"
    "IPI0:       246        143        230        259        212        204        285        216       Rescheduling interrupts\n"
    "Err:          0\n";
#elif defined(SOC_RK3288)
static const char FALLBACK_INTERRUPTS[] =
    "           CPU0       CPU1       CPU2       CPU3\n"
    "  1:      12345      11111      10000       9999     GIC-0  29 Level     arch_timer\n"
    "  2:        512          0          0          0     GIC-0  33 Level     uart-pl011\n"
    " 32:       2508          0          0          0     GIC-0  74 Edge      dwc3\n"
    " 33:         23          0          0          0     GIC-0  77 Edge      fe210000.sata\n"
    " 34:          2          0          0          0     GIC-0  78 Edge      fea10000.dma-controller\n"
    " 35:       2902          0          0          0     GIC-0  75 Edge      ff0c0000.dwmmc\n"
    " 36:        107          0          0          0     GIC-0  73 Edge      ff0f0000.dwmmc\n"
    " 37:        283          0          0          0     GIC-0  79 Edge      ttyS0\n";
static const char DT_PCB_REV[] = "B";
#endif

static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;
static char *fake_interrupts_content = NULL;
static int fake_interrupts_generation_failed = 0;

/* Returns an fd (from an anonymous memfd, so nothing touches the real
 * filesystem) with freshly-seeked fake /proc/interrupts content, or -1 if
 * generation isn't possible this run (in which case FALLBACK_INTERRUPTS
 * above answers instead). Content is generated once per process and
 * cached — every caller gets its own independent fd/position over the same
 * cached text, matching normal open() semantics for multiple readers. */
/* Hand the caller a read-only fd over content this process holds in memory.
 *
 * A memfd rather than a temp file: nothing lands on a read-only rootfs, it needs
 * no cleanup, and it is mmap-able for callers that want that. len 0 means "take
 * strlen", which is what every text property wants; the binary properties pass
 * their own length because they contain NULs. */
static int blob_fd(const char *name, const char *content, size_t len) {
    if (!content) return -1;
    if (len == 0) len = strlen(content);

    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) return -1;

    if (len && get_real_write()(fd, content, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int get_fake_interrupts_fd(void) {
    pthread_mutex_lock(&gen_lock);
    if (!fake_interrupts_content && !fake_interrupts_generation_failed) {
        fake_interrupts_content = build_fake_interrupts();
        if (!fake_interrupts_content) fake_interrupts_generation_failed = 1;
    }
    char *content = fake_interrupts_content;
    pthread_mutex_unlock(&gen_lock);

    /* Generation is what normally answers this read. FALLBACK_INTERRUPTS is the
     * last resort, for when the real /proc/interrupts cannot be read at all or
     * holds no IRQ that survives the affinity probe. It used to be a file the
     * builders wrote; it is compiled in so it cannot drift from what the parser
     * above expects, or be forgotten by a builder. */
    if (!content) return blob_fd("fake-proc-interrupts", FALLBACK_INTERRUPTS, 0);
    return blob_fd("fake-proc-interrupts", content, 0);
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

#if defined(SOC_RK3588)
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
#else
/* The logging above exists for the RK3588 bring-up, where it answered which
 * devicetree properties Engine actually reads. Compiled out elsewhere rather
 * than deleted: the call sites below stay identical for both SoCs, and
 * `if (0) ...` costs nothing. */
#define is_dt_path(p) 0
#define log_dt_access(fn, orig, mapped, ok, err) ((void)0)
#endif

/* Which devicetree properties this shim serves from /root/fake-dt, per SoC.
 *
 * dtshim remaps unconditionally: if a path is listed here, the file it points at
 * must exist in the guest, because a missing target turns a working read into
 * ENOENT. That loudness is deliberate -- a silently-absent property is far harder
 * to diagnose than a failed read -- so the builders write exactly this set, and
 * the two lists are kept whole per SoC rather than factored into a common part
 * plus differences. What each SoC remaps is then readable in one place.
 *
 * The RK3588 panel node lives at dsi@... or edp-panel; RK3288's at
 * mipi@ff960000. RK3288 additionally has an az01-pcb-rev and a
 * chosen/inmusic,internal-sd-fitted that RK3588 has no property for. */
/* Exactly one of `to` and `blob` is set. `to` is a path to open instead; `blob`
 * is content this process serves from memory, with `len` 0 meaning strlen. */
struct dt_remap {
    const char *from;
    const char *to;
    const char *blob;
    size_t len;
};

static const struct dt_remap DT_REMAPS[] = {
    /* Identity: written per build, so served from files. */
    {"/sys/firmware/devicetree/base/inmusic,product-code",
                                    "/root/fake-dt/inmusic,product-code", NULL, 0},
    {"/sys/firmware/devicetree/base/serial-number",
                                    "/root/fake-dt/serial-number",        NULL, 0},
#if defined(SOC_RK3588)
    /* Four nodes, one value: which panel node exists depends on the display this
     * image is built for, and dtshim answers whichever Engine probes. */
    {"/sys/firmware/devicetree/base/dsi@fde20000/panel@0/rotation",  NULL, DT_U32_ZERO, 4},
    {"/sys/firmware/devicetree/base/dsi@fde30000/panel@0/rotation",  NULL, DT_U32_ZERO, 4},
    {"/sys/firmware/devicetree/base/mipi@ff960000/panel@0/rotation", NULL, DT_U32_ZERO, 4},
    {"/sys/firmware/devicetree/base/edp-panel/rotation",             NULL, DT_U32_ZERO, 4},
#elif defined(SOC_RK3288)
    {"/sys/firmware/devicetree/base/inmusic,az01-pcb-rev", NULL, DT_PCB_REV, 0},
    {"/sys/firmware/devicetree/base/mipi@ff960000/panel@0/rotation", NULL, DT_U32_ZERO, 4},
    {"/sys/firmware/devicetree/base/chosen/inmusic,internal-sd-fitted",
                                                          NULL, DT_U32_ZERO, 4},
#else
# error "define SOC_RK3288 or SOC_RK3588 so dtshim knows which properties to serve"
#endif
    /* Not a devicetree property, and the same on every SoC: /dev/mem is remapped
     * to a plain file so a mmap of it fails cleanly rather than handing out real
     * physical memory, which is what the hardware anti-clone check probes. Left as
     * a file rather than a blob so that failure mode does not move.
     *
     * /proc/interrupts is deliberately absent from this table: the wrappers answer
     * it from get_fake_interrupts_fd() before remap() is consulted, generating it
     * at runtime and falling back to FALLBACK_INTERRUPTS, so there is no path to
     * remap it to. */
    {"/dev/mem",         "/root/fake-dev-mem", NULL, 0},
};

static const struct dt_remap *dt_lookup(const char *path) {
    if (!path) return NULL;
    for (size_t i = 0; i < sizeof(DT_REMAPS) / sizeof(DT_REMAPS[0]); i++)
        if (strcmp(path, DT_REMAPS[i].from) == 0)
            return &DT_REMAPS[i];
    return NULL;
}

#ifdef SOC_RK3288
static const char *remap_secure_efuse(const char *path, char *mapped,
                                      size_t mapped_size);
static __thread char g_secure_efuse_path[PATH_MAX];
#endif

/* The path to open for this request: a remap target, or the original. An entry
 * served from a blob has no path, and callers check for that first. */
static const char *remap(const char *path) {
    if (!path) return NULL;
    const struct dt_remap *e = dt_lookup(path);
    if (e && e->to) return e->to;
#ifdef SOC_RK3288
    return remap_secure_efuse(path, g_secure_efuse_path,
                              sizeof(g_secure_efuse_path));
#else
    return path;
#endif
}

#ifdef SOC_RK3288
/* Engine validates RK3288 products against the secure eFuse before Qt starts.
 * The generic QEMU machine has no RK3288 eFuse platform device, so redirect
 * that sysfs subtree to the fixed nvmem fixtures written by write_fake_dt().
 * Interposing open() alone is too late: glibc resolves every parent component
 * and rejects the missing platform directory first. */
static const char *remap_secure_efuse(const char *path, char *mapped,
                                      size_t mapped_size) {
    static const struct {
        const char *real_prefix;
        const char *fake_prefix;
    } EFUSE_REMAPS[] = {
        {"/sys/devices/platform/ffb10000.efuse",
         "/root/fake-dt/ffb10000.efuse"},
        {"/sys/devices/platform/ffb40000.efuse",
         "/root/fake-dt/ffb40000.efuse"},
    };

    if (!path) return path;
    for (size_t i = 0; i < sizeof(EFUSE_REMAPS) / sizeof(EFUSE_REMAPS[0]); i++) {
        size_t prefix_len = strlen(EFUSE_REMAPS[i].real_prefix);
        if (strncmp(path, EFUSE_REMAPS[i].real_prefix, prefix_len) != 0 ||
            (path[prefix_len] != '\0' && path[prefix_len] != '/')) {
            continue;
        }
        if (snprintf(mapped, mapped_size, "%s%s", EFUSE_REMAPS[i].fake_prefix,
                     path + prefix_len) >= (int)mapped_size) {
            return path;
        }
        return mapped;
    }
    return path;
}

char *__realpath_chk(const char *path, char *resolved, size_t resolved_size) {
    static realpath_chk_t real_realpath_chk = NULL;
    char mapped[PATH_MAX];

    if (!real_realpath_chk) {
        real_realpath_chk =
            (realpath_chk_t)dlsym(RTLD_NEXT, "__realpath_chk");
    }
    return real_realpath_chk(
        remap_secure_efuse(path, mapped, sizeof(mapped)), resolved,
        resolved_size);
}

/* std::filesystem::canonical() enumerates each parent directory before it ever
 * asks libc to open the final path. Advertise the synthetic platform entry at
 * that boundary; subsequent opens are redirected by remap(). */
struct dirent64 *readdir64(DIR *dirp) {
    static readdir64_t real_readdir64 = NULL;
    static __thread int injected_fd = -1;
    static __thread int injected_entries = 0;
    static __thread struct dirent64 fake_entry;
    static const char *fake_names[] = {"ffb10000.efuse", "ffb40000.efuse"};

    if (!real_readdir64) {
        real_readdir64 = (readdir64_t)dlsym(RTLD_NEXT, "readdir64");
    }
    int fd = dirfd(dirp);
    if (injected_fd != fd) {
        char fd_path[64];
        char target[PATH_MAX];
        snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
        ssize_t len = readlink(fd_path, target, sizeof(target) - 1);
        if (len >= 0) target[len] = '\0';
        if (len >= 0 && strcmp(target, "/sys/devices/platform") == 0) {
            injected_fd = fd;
            injected_entries = 0;
        }
    }
    if (injected_fd == fd &&
        injected_entries < (int)(sizeof(fake_names) / sizeof(fake_names[0]))) {
        const char *name = fake_names[injected_entries];
        memset(&fake_entry, 0, sizeof(fake_entry));
        fake_entry.d_ino = 1;
        fake_entry.d_reclen =
            (unsigned short)(offsetof(struct dirent64, d_name) +
                             strlen(name) + 1);
        fake_entry.d_type = DT_DIR;
        strcpy(fake_entry.d_name, name);
        injected_entries++;
        return &fake_entry;
    }
    return real_readdir64(dirp);
}

DIR *opendir(const char *path) {
    static opendir_t real_opendir = NULL;
    if (!real_opendir) real_opendir = (opendir_t)dlsym(RTLD_NEXT, "opendir");
    return real_opendir(remap(path));
}
#endif

/* An fd over a blob entry's content, or -1 if this path is not served that way. */
static int dt_blob_fd(const char *path) {
    const struct dt_remap *e = dt_lookup(path);
    if (!e || !e->blob) return -1;
    return blob_fd("fake-dt-property", e->blob, e->len);
}

/* Existence checks, which a caller may do before ever opening the file.
 *
 * MPC does exactly that: it imports access() and, notably, no member of the stat
 * family at all, so a guarded read of inmusic,product-code never reaches the open
 * wrappers below -- the guard fails on the real sysfs path and the read is
 * abandoned. That is invisible from Engine, which opens directly, and it is why an
 * emulated MPC identified as <Unknown> despite the shim serving the property
 * correctly to anything that asked for it by opening.
 *
 * Deliberately not interposing the stat family *in the generic wrappers below*.
 * It is the riskiest family to interpose -- stat/stat64/__xstat/__xstat64/statx
 * differ by glibc version and by _FILE_OFFSET_BITS, so getting it wrong breaks
 * every caller rather than only the ones that wanted a devicetree.
 *
 * The one exception is the RK3288 eFuse path (see the SOC_RK3288 block above):
 * Engine's board validation stats that synthetic path before opening it, and
 * glibc ≥ 2.34 implements stat()/lstat()/fstatat() via the time64 symbols rather
 * than statx(), so the SOC_RK3288 interposition of statx plus the three time64
 * symbols is what makes the redirect reachable. Engine before 5.0 has a 32-bit
 * time_t and uses stat64/lstat64/fstatat64 instead, which redirect only the
 * eFuse subtree. Everything else stays out of it.
 */
int access(const char *path, int mode) {
    static access_t real_access = NULL;
    if (!real_access) real_access = (access_t)dlsym(RTLD_NEXT, "access");
    if (!path) return real_access(path, mode);

    const struct dt_remap *e = dt_lookup(path);
    /* A blob-backed property has no file anywhere to consult, so answer from the
     * table: it is there and readable, and nothing the shim serves is writable or
     * executable. */
    if (e && e->blob) {
        if (mode & (W_OK | X_OK)) { errno = EACCES; return -1; }
        if (is_dt_path(path)) log_dt_access("access", path, "<in-memory>", 1, 0);
        return 0;
    }
    const char *mapped = remap(path);
    int rc = real_access(mapped, mode);
    if (is_dt_path(path)) log_dt_access("access", path, mapped, rc == 0, errno);
    return rc;
}

/* Same remap for the at-relative form. Only AT_FDCWD with an absolute path can
 * name a devicetree property -- every path the table holds is absolute -- so
 * anything else goes straight through rather than being second-guessed. */
int faccessat(int dirfd, const char *path, int mode, int flags) {
    static faccessat_t real_faccessat = NULL;
    if (!real_faccessat) real_faccessat = (faccessat_t)dlsym(RTLD_NEXT, "faccessat");
    if (path && path[0] == '/') {
        const struct dt_remap *e = dt_lookup(path);
        if (e && e->blob) {
            if (mode & (W_OK | X_OK)) { errno = EACCES; return -1; }
            if (is_dt_path(path)) log_dt_access("faccessat", path, "<in-memory>", 1, 0);
            return 0;
        }
        const char *mapped = remap(path);
        if (mapped != path) {
            int rc = real_faccessat(dirfd, mapped, mode, flags);
            if (is_dt_path(path)) log_dt_access("faccessat", path, mapped, rc == 0, errno);
            return rc;
        }
    }
    return real_faccessat(dirfd, path, mode, flags);
}

#ifdef SOC_RK3288
int openat(int dirfd, const char *path, int flags, ...) {
    static openat_t real_openat = NULL;
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (!real_openat) real_openat = (openat_t)dlsym(RTLD_NEXT, "openat");
    const char *mapped = path && path[0] == '/' ? remap(path) : path;
    return real_openat(dirfd, mapped, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    static openat_t real_openat64 = NULL;
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (!real_openat64) real_openat64 = (openat_t)dlsym(RTLD_NEXT, "openat64");
    const char *mapped = path && path[0] == '/' ? remap(path) : path;
    return real_openat64(dirfd, mapped, flags, mode);
}

int statx(int dirfd, const char *path, int flags, unsigned int mask,
          struct statx *buf) {
    static statx_t real_statx = NULL;
    if (!real_statx) real_statx = (statx_t)dlsym(RTLD_NEXT, "statx");
    const char *mapped = path && path[0] == '/' ? remap(path) : path;
    return real_statx(dirfd, mapped, flags, mask, buf);
}

/* glibc's stat()/lstat()/fstatat() do not call the statx libc function -- each
 * is implemented by its own time64 symbol (__stat64_time64, __lstat64_time64,
 * __fstatat64_time64) which issues the statx syscall directly. Engine imports
 * all three, so remap each or the efuse metadata check never sees the synthetic
 * path. The buffer is an opaque stat struct; pass it through untouched. */
typedef int (*stat64_time64_t)(const char *, void *);
typedef int (*fstatat64_time64_t)(int, const char *, void *, int);

int __stat64_time64(const char *path, void *buf) {
    static stat64_time64_t real_stat64_time64 = NULL;
    if (!real_stat64_time64)
        real_stat64_time64 = (stat64_time64_t)dlsym(RTLD_NEXT, "__stat64_time64");
    return real_stat64_time64(remap(path), buf);
}

int __lstat64_time64(const char *path, void *buf) {
    static stat64_time64_t real_lstat64_time64 = NULL;
    if (!real_lstat64_time64)
        real_lstat64_time64 =
            (stat64_time64_t)dlsym(RTLD_NEXT, "__lstat64_time64");
    return real_lstat64_time64(remap(path), buf);
}

int __fstatat64_time64(int dirfd, const char *path, void *buf, int flags) {
    static fstatat64_time64_t real_fstatat64_time64 = NULL;
    if (!real_fstatat64_time64)
        real_fstatat64_time64 =
            (fstatat64_time64_t)dlsym(RTLD_NEXT, "__fstatat64_time64");
    const char *mapped = path && path[0] == '/' ? remap(path) : path;
    return real_fstatat64_time64(dirfd, mapped, buf, flags);
}

/* Engine before 5.0 (4.3.4, Qt5 on a Buildroot glibc 2.36) was built with a
 * 32-bit time_t, so it imports stat64/lstat64/fstatat64 and plain realpath()
 * instead of the time64 names and __realpath_chk above. It runs the same eFuse
 * check, and without these the stat of the synthetic path misses the fixtures
 * and Engine exits cleanly. Exporting them costs a 5.x guest nothing: its Engine
 * never calls these names.
 *
 * These remap only the eFuse subtree, not every devicetree path as remap()
 * does. RK3288 MPC builds preload this shim too and share the 32-bit time_t,
 * so a full remap here would change what MPC sees when it stats a devicetree
 * path, for no reason that anything has shown. */
typedef int (*stat64_t)(const char *, struct stat64 *);
typedef int (*fstatat64_t)(int, const char *, struct stat64 *, int);
typedef char *(*realpath_t)(const char *, char *);

int stat64(const char *path, struct stat64 *buf) {
    static stat64_t real_stat64 = NULL;
    char mapped[PATH_MAX];
    if (!real_stat64) real_stat64 = (stat64_t)dlsym(RTLD_NEXT, "stat64");
    return real_stat64(remap_secure_efuse(path, mapped, sizeof(mapped)), buf);
}

int lstat64(const char *path, struct stat64 *buf) {
    static stat64_t real_lstat64 = NULL;
    char mapped[PATH_MAX];
    if (!real_lstat64) real_lstat64 = (stat64_t)dlsym(RTLD_NEXT, "lstat64");
    return real_lstat64(remap_secure_efuse(path, mapped, sizeof(mapped)), buf);
}

int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags) {
    static fstatat64_t real_fstatat64 = NULL;
    char mapped[PATH_MAX];
    if (!real_fstatat64) real_fstatat64 = (fstatat64_t)dlsym(RTLD_NEXT, "fstatat64");
    const char *p = path && path[0] == '/' ? remap_secure_efuse(path, mapped, sizeof(mapped)) : path;
    return real_fstatat64(dirfd, p, buf, flags);
}

char *realpath(const char *path, char *resolved) {
    static realpath_t real_realpath = NULL;
    char mapped[PATH_MAX];
    if (!real_realpath) real_realpath = (realpath_t)dlsym(RTLD_NEXT, "realpath");
    return real_realpath(remap_secure_efuse(path, mapped, sizeof(mapped)), resolved);
}

ssize_t readlink(const char *path, char *buf, size_t buf_size) {
    static readlink_t real_readlink = NULL;
    if (!real_readlink) real_readlink = (readlink_t)dlsym(RTLD_NEXT, "readlink");
    return real_readlink(remap(path), buf, buf_size);
}
#endif

int open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);

    if (path && strcmp(path, "/proc/interrupts") == 0) {
        int fd = get_fake_interrupts_fd();
        if (fd >= 0) return fd;
    }
    int blob = dt_blob_fd(path);
    if (blob >= 0) {
        if (is_dt_path(path)) log_dt_access("open", path, "<in-memory>", 1, 0);
        return blob;
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
    int blob = dt_blob_fd(path);
    if (blob >= 0) {
        if (is_dt_path(path)) log_dt_access("open64", path, "<in-memory>", 1, 0);
        return blob;
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
    int blob = dt_blob_fd(path);
    if (blob >= 0) {
        FILE *bf = fdopen(blob, mode);
        if (bf) {
            if (is_dt_path(path)) log_dt_access("fopen", path, "<in-memory>", 1, 0);
            return bf;
        }
        close(blob);
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
    int blob = dt_blob_fd(path);
    if (blob >= 0) {
        FILE *bf = fdopen(blob, mode);
        if (bf) {
            if (is_dt_path(path)) log_dt_access("fopen64", path, "<in-memory>", 1, 0);
            return bf;
        }
        close(blob);
    }
    const char *mapped = remap(path);
    FILE *f = real_fopen64(mapped, mode);
    if (is_dt_path(path)) log_dt_access("fopen64", path, mapped, f != NULL, errno);
    return f;
}
