/* The probes of dtshim: everything Engine reads from a path the emulated
 * machine does not really have. What each expects is dtshim's own contract, as
 * DT_REMAPS, write_fake_dt.sh and shims/dtshim/README.md state it. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fake.h"

#define DT "/sys/firmware/devicetree/base/"

static int soc_is(const char *soc) { return strcmp(cfg.soc, soc) == 0; }

/* --- identity ---------------------------------------------------------------
 * Engine opens the product code directly. MPC checks it with access() first and
 * gives up when that fails, which is why both families run for every app. */
void probe_identity(void) {
    run_family("identity.product-code", READ_VARIANTS, DT "inmusic,product-code", check_text,
               (void *)cfg.code);
    run_family("identity.product-code.exists", ACCESS_VARIANTS, DT "inmusic,product-code",
               NULL, NULL);
    run_family("identity.serial-number", READ_VARIANTS, DT "serial-number", check_text, NULL);
    run_family("identity.serial-number.exists", ACCESS_VARIANTS, DT "serial-number", NULL,
               NULL);
}

/* --- devicetree -------------------------------------------------------------
 * The properties dtshim serves from itself, per SoC. */
static int check_rev(const char *buf, long n, char *detail, size_t dl, void *expected) {
    (void)n;
    snprintf(detail, dl, "\"%.20s\"", buf);
    return strcmp(buf, expected) == 0;
}

void probe_devicetree(void) {
    static const char *const rk3588[] = {"dsi@fde20000/panel@0/rotation",
                                         "dsi@fde30000/panel@0/rotation",
                                         "mipi@ff960000/panel@0/rotation",
                                         "edp-panel/rotation", NULL};
    static const char *const rk3566[] = {"spi@fe640000/display@0/rotation",
                                         "inmusic,panel-rotation", NULL};
    static const char *const rk3288[] = {"mipi@ff960000/panel@0/rotation",
                                         "chosen/inmusic,internal-sd-fitted", NULL};
    const char *const *cells = soc_is("rk3588") ? rk3588 : soc_is("rk3566") ? rk3566
                               : soc_is("rk3288") ? rk3288 : NULL;
    if (!cells) {
        t_skip("devicetree", "no --soc given");
        return;
    }
    char name[160], path[256];
    for (; *cells; cells++) {
        snprintf(name, sizeof(name), "devicetree.%s", *cells);
        snprintf(path, sizeof(path), DT "%s", *cells);
        run_family(name, READ_VARIANTS, path, check_zero_cell, NULL);
    }
    if (soc_is("rk3288"))
        run_family("devicetree.inmusic,az01-pcb-rev", READ_VARIANTS, DT "inmusic,az01-pcb-rev",
                   check_rev, "B");
    if (soc_is("rk3566"))
        run_family("devicetree.inmusic,az07-pcb-rev", READ_VARIANTS, DT "inmusic,az07-pcb-rev",
                   check_rev, "A");
}

/* --- drum content ------------------------------------------------------------
 * A boolean the Core Drum Module's app and units check for, so presence is the
 * value: there for LDMF, and absent everywhere else, where an LDMD app reading it
 * would take the Core's storage layout. Every RK3288 app gets the same dtshim. */
void probe_drumcontent(void) {
    const char *path = DT "chosen/inmusic,emmc-drum-content";
    const char *name = "drumcontent.chosen/inmusic,emmc-drum-content";
    if (cfg.code && strcmp(cfg.code, "LDMF") == 0) {
        run_family(name, ACCESS_VARIANTS, path, NULL, NULL);
        return;
    }
    int (*acc)(const char *, int) = entry("access");
    if (acc && acc(path, F_OK) == 0)
        t_fail(name, "present on %s, which has no eMMC drum content",
               cfg.code ? cfg.code : "this product");
    else
        t_ok(name, "absent: %s", strerror(errno));
}

/* --- /dev/mem ---------------------------------------------------------------
 * The anti-clone check maps physical memory. It has to get a mapping, and zeros
 * from it: an empty stand-in file maps fine and then raises SIGBUS on the first
 * read, which is how MPC crash-looped. */
static sigjmp_buf bus_jmp;
static void on_bus(int sig) { (void)sig; siglongjmp(bus_jmp, 1); }

/* 1 if the page reads as zero, 0 if not, -1 if reading it faults. */
static int page_reads_zero(const volatile unsigned char *p) {
    struct sigaction sa, old_bus, old_segv;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_bus;
    sigaction(SIGBUS, &sa, &old_bus);
    sigaction(SIGSEGV, &sa, &old_segv);
    int result = -1;
    if (sigsetjmp(bus_jmp, 1) == 0) result = (p[0] | p[0x50] | p[4095]) == 0;
    sigaction(SIGBUS, &old_bus, NULL);
    sigaction(SIGSEGV, &old_segv, NULL);
    return result;
}

static void map_check(const char *name, void *p) {
    if (p == MAP_FAILED) {
        t_fail(name, "mmap: %s", strerror(errno));
        return;
    }
    int zero = page_reads_zero(p);
    if (zero < 0) t_fail(name, "mapped, then faulted on the first read");
    else if (!zero) t_fail(name, "mapped, but not zeros");
    else t_ok(name, "maps and reads zeros");
    munmap(p, 4096);
}

void probe_devmem(void) {
    static const char *const opens[] = {"open", "open64", NULL};
    void *(*map64)(void *, size_t, int, int, int, long long) = entry("mmap64");
    void *(*map32)(void *, size_t, int, int, int, long) = entry("mmap");
    int done = 0;
    for (int i = 0; opens[i] && !done; i++) {
        int (*open_fn)(const char *, int, ...) = entry(opens[i]);
        if (!open_fn || app_imports(opens[i]) == 0) continue;
        char name[64];
        snprintf(name, sizeof(name), "devmem.%s", opens[i]);
        /* Read-only: the stand-in is a file on a root the firmware mounts
         * read-only, and Engine gets its mapping there. */
        int fd = open_fn("/dev/mem", O_RDONLY | O_SYNC);
        if (fd < 0) {
            t_fail(name, "%s", strerror(errno));
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
            /* Not mapped: this is the machine's real physical memory. */
            t_fail(name, "reached the real /dev/mem, dtshim did not redirect it");
            close(fd);
            continue;
        }
        t_ok(name, "a stand-in, not the device");
        done = 1;
        if (map64) {
            map_check("devmem.map", map64(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0));
            /* RK3288 register space, where MPC and Engine look. */
            if (soc_is("rk3288"))
                map_check("devmem.map-registers",
                          map64(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0xff7e0000LL));
        }
        /* SC5000 1.2.0 maps the same register through the 32-bit call, where
         * the offset arrives negative. Only the legacy build of dtshim, which
         * interposes mmap, answers that. */
        if (map32 && shim_exporting("mmap") && sizeof(long) == 4)
            map_check("devmem.map-registers-32bit",
                      map32(NULL, 4096, PROT_READ, MAP_SHARED, fd, (long)0xff7e0000UL));
        close(fd);
    }
    if (!done) t_fail("devmem", "/dev/mem did not open as a stand-in");
}

/* --- /proc/interrupts -------------------------------------------------------
 * Engine looks six IRQs up by name and aborts without them, then sets each
 * one's affinity from a shell: sh -c "echo ... > /proc/irq/N/smp_affinity_list".
 * The shell is a new process, so the shim has to answer there too. */
static const char *const IRQ_NAMES[] = {"dwc3", "fe210000.sata", "fea10000.dma-controller",
                                        "ff0c0000.dwmmc", "ff0f0000.dwmmc", "ttyS0"};
#define NIRQS (sizeof(IRQ_NAMES) / sizeof(IRQ_NAMES[0]))
static long irq_numbers[NIRQS];

/* The IRQ whose line ends in name, or -1. */
static long irq_named(const char *text, const char *name) {
    size_t len = strlen(name);
    for (const char *row = text; row && *row; row = strchr(row, '\n'), row += row ? 1 : 0) {
        const char *end = strchr(row, '\n');
        size_t row_len = end ? (size_t)(end - row) : strlen(row);
        if (row_len > len && strncmp(row + row_len - len, name, len) == 0 &&
            row[row_len - len - 1] == ' ')
            return strtol(row, NULL, 10);
    }
    return -1;
}

static int check_interrupts(const char *buf, long n, char *detail, size_t dl, void *unused) {
    (void)n; (void)unused;
    for (size_t i = 0; i < NIRQS; i++)
        if ((irq_numbers[i] = irq_named(buf, IRQ_NAMES[i])) < 0) {
            snprintf(detail, dl, "no line for %s", IRQ_NAMES[i]);
            return 0;
        }
    snprintf(detail, dl, "all six names");
    return 1;
}

void probe_interrupts(void) {
    if (!run_family("interrupts.names", READ_VARIANTS, "/proc/interrupts", check_interrupts,
                    NULL))
        return;
    int accepted = 0;
    char failed[256] = "";
    for (size_t i = 0; i < NIRQS; i++) {
        /* Written back as it stands, so a shim that lets the write through
         * changes nothing on the machine this runs on. */
        char path[96], current[64] = "0", cmd[256];
        snprintf(path, sizeof(path), "/proc/irq/%ld/smp_affinity_list", irq_numbers[i]);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(current, sizeof(current), f)) current[strcspn(current, "\n")] = '\0';
            fclose(f);
        }
        snprintf(cmd, sizeof(cmd), "echo '%s' > %s 2>/dev/null", current, path);
        int status = system(cmd);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) accepted++;
        else snprintf(failed + strlen(failed), sizeof(failed) - strlen(failed), " %s(irq %ld)",
                      IRQ_NAMES[i], irq_numbers[i]);
    }
    if (accepted == (int)NIRQS) t_ok("interrupts.affinity", "six writes from sh accepted");
    else t_fail("interrupts.affinity", "refused:%s", failed);
}

/* --- eFuse ------------------------------------------------------------------
 * Signed RK3288 firmware validates the board against two eFuse nvmem files
 * before Qt starts. It stats the path, canonicalises it, which walks the parent
 * directory, and reads it. Each step is a different libc family. */
#define PLATFORM "/sys/devices/platform"

static int check_secure(const char *buf, long n, char *detail, size_t dl, void *unused) {
    (void)unused;
    size_t len = cfg.code ? strlen(cfg.code) : 0;
    snprintf(detail, dl, "%ld bytes, starts \"%.8s\"", n, buf);
    return n > 0 && (!cfg.code || ((size_t)n >= len && memcmp(buf, cfg.code, len) == 0));
}

static int check_cpuid(const char *buf, long n, char *detail, size_t dl, void *unused) {
    (void)unused;
    snprintf(detail, dl, "%ld bytes, \"%.14s\" at 7", n, n > 7 ? buf + 7 : "");
    return n >= 13 && memcmp(buf + 7, "RK3288", 6) == 0;
}

static void efuse_listing(void) {
    static const char *const readers[] = {"readdir64", "readdir", NULL};
    DIR *(*opendir_fn)(const char *) = entry("opendir");
    for (int i = 0; readers[i]; i++) {
        void *(*read_fn)(DIR *) = entry(readers[i]);
        if (!opendir_fn || !read_fn) continue;
        char name[64];
        snprintf(name, sizeof(name), "efuse.listing.%s", readers[i]);
        DIR *d = opendir_fn(PLATFORM);
        int seen = 0;
        if (d) {
            /* d_name sits at a different offset in each layout. */
            for (void *e; (e = read_fn(d));) {
                const char *entry_name = strcmp(readers[i], "readdir64") == 0
                                             ? ((struct dirent64 *)e)->d_name
                                             : ((struct dirent *)e)->d_name;
                seen += !strcmp(entry_name, "ffb10000.efuse") ||
                        !strcmp(entry_name, "ffb40000.efuse");
            }
            closedir(d);
        }
        int claimed = shim_exporting(readers[i]) && app_imports(readers[i]) != 0;
        if (seen == 2) t_ok(name, "both eFuse devices listed under " PLATFORM);
        else if (claimed) t_fail(name, "%d of 2 eFuse devices listed under " PLATFORM, seen);
        else t_skip(name, "%d of 2 listed, and %s is not both interposed and imported", seen,
                    readers[i]);
    }
}

void probe_efuse(void) {
    if (!soc_is("rk3288")) {
        t_skip("efuse", "only RK3288 firmware checks it");
        return;
    }
    /* Unsigned firmware never looks, and an instance built before the fixtures
     * existed boots without them. */
    if (access("/root/fake-dt/ffb10000.efuse", F_OK) != 0) {
        t_skip("efuse", "this rootfs was built without eFuse fixtures");
        return;
    }
    const char *secure = PLATFORM "/ffb10000.efuse/rockchip-efuse/nvmem";
    const char *cpuid = PLATFORM "/ffb40000.efuse/rockchip-efuse/nvmem";
    run_family("efuse.secure.stat", STAT_VARIANTS, secure, NULL, NULL);
    run_family("efuse.cpuid.stat", STAT_VARIANTS, cpuid, NULL, NULL);
    run_family("efuse.secure.canonical", PATH_VARIANTS, secure, NULL, NULL);
    run_family("efuse.cpuid.canonical", PATH_VARIANTS, cpuid, NULL, NULL);
    efuse_listing();
    run_family("efuse.secure.read", READ_VARIANTS, secure, check_secure, NULL);
    run_family("efuse.cpuid.read", READ_VARIANTS, cpuid, check_cpuid, NULL);
}

/* --- battery ----------------------------------------------------------------
 * Prime GO, Prime GO+ and Mixstream Pro GO poll these paths directly. */
void probe_battery(void) {
    static const char *const battery_codes[] = {"JP11", "JP11S", "NH10", NULL};
    int has_battery = 0;
    for (int i = 0; cfg.code && battery_codes[i]; i++)
        has_battery |= strcmp(cfg.code, battery_codes[i]) == 0;
    if (!soc_is("rk3288") || !has_battery) {
        t_skip("battery", "%s has no battery to fake", cfg.code ? cfg.code : "this product");
        return;
    }
    if (access("/root/fake-power", F_OK) != 0) {
        t_skip("battery", "this rootfs was built without battery fixtures");
        return;
    }
    static const struct { const char *path, *value; } files[] = {
        {"/sys/class/power_supply/sbs-3-000b/present", "1"},
        {"/sys/class/power_supply/sbs-3-000b/capacity", "100"},
        {"/sys/class/power_supply/sbs-3-000b/status", "Full"},
        {"/sys/class/power_supply/az01-ac-power/online", "1"},
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char name[160];
        snprintf(name, sizeof(name), "battery.%s", strrchr(files[i].path, '/') + 1);
        run_family(name, READ_VARIANTS, files[i].path, check_line, (void *)files[i].value);
    }
}
