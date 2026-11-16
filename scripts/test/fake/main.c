/* A fake Engine: a small program that makes the calls Engine makes at QEngine's
 * shims, and says for each whether it got what Engine needs.
 *
 * Booting the real application is the only complete test of a shim, and it
 * takes minutes per firmware version and a hand on the touchscreen. Most of
 * what a shim change can break is narrower than that: whether the guest's
 * loader accepts the shim at all, and whether each call Engine makes at it
 * still gets its answer. This program asks those questions in a second or two,
 * inside the instance's own rootfs, with the unit's own environment and
 * LD_PRELOAD. check.sh puts it there and runs it.
 *
 * It is installed under the name Engine because quitshim only arms itself in a
 * process of that name.
 *
 * WHICH LIBC ENTRY POINTS
 *
 * The same read reaches libc through a different symbol on each firmware era:
 * open or open64, stat64 or __stat64_time64 or __xstat64, realpath or
 * __realpath_chk, depending on the glibc and the flags Engine was built with.
 * A shim that interposes the wrong one loads and does nothing. So this program
 * does not rely on how it was compiled. It looks every entry point up by name,
 * in the order a call from the application would be resolved (preloads first),
 * and tries each one that exists. One binary per architecture then covers
 * every era, and it is built on Debian jessie so that glibc 2.22, SC5000
 * 1.0.1's, will load it.
 *
 * Which of those results matter is read off the firmware. With --real, the
 * program reads the real application's imports (and those of libstdc++ and
 * libaz0x-*, which do some of its file access for it). Each read then gets one
 * line per entry point and one for the family, and the family passes when an
 * entry point the application imports returns the right thing.
 *
 * A single entry point that does not work is reported as skipped, never
 * failed. Engine imports openat64 and reads no devicetree through it, and
 * which entry point it uses for which path cannot be read off the binary. So
 * the fake alone cannot tell a broken interposer from one nobody needed, or
 * notice one being deleted. The baseline check.sh saves can: a result that was
 * ok and is now skipped is reported as a change.
 *
 * WHAT IT CANNOT SAY
 *
 * The probes reproduce the call shapes the shims were written against, as
 * their own comments record them. They know nothing a shim's author did not,
 * so a firmware that reads something new still has to be found by booting it.
 * Nothing here touches Qt or the display: drmatomic, cursorshim, qmlrotate,
 * MONTERO and the touch bridges are not covered.
 *
 * MODULAR BY APPLICATION
 *
 * Probes are rows in PROBES below, each with the applications it is for. Those
 * for any_app are what MPC and the other inMusic applications share with Engine
 * (the loader, the devicetree identity, /dev/mem). An MPC-specific probe is one
 * more row, one more file and one more applies function.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fake.h"

struct config cfg = {"engine", "", NULL, NULL};

/* Which applications a probe is for. */
static int any_app(void) { return 1; }
static int engine(void) { return strcmp(cfg.app, "engine") == 0; }
/* dtshim's RK3566 table exists for MPC Sample and HeadRush VX5, not Engine. */
static int engine_or_rk3566(void) { return engine() || strcmp(cfg.soc, "rk3566") == 0; }

static const struct probe PROBES[] = {
    {"loader", "every LD_PRELOAD entry loaded and has its symbols", probe_loader, any_app},
    {"surface", "which shim exports the application imports (comments only)", probe_surface, any_app},
    {"identity", "product code and serial number from the devicetree", probe_identity, any_app},
    {"devmem", "/dev/mem opens and maps as zeros", probe_devmem, any_app},
    {"devicetree", "panel rotation and board properties for the SoC", probe_devicetree, engine_or_rk3566},
    {"interrupts", "/proc/interrupts names six IRQs and takes affinity writes", probe_interrupts, engine},
    {"efuse", "RK3288 eFuse files, by stat, canonical path and read", probe_efuse, engine},
    {"battery", "full battery on AC for JP11, JP11S and NH10", probe_battery, engine},
    {"tee", "OP-TEE attestation, where the application links libteec", probe_tee, engine},
    {"alsa", "card name, plug routing, ring depth and playback", probe_alsa, engine},
    {"midi", "control surface found, numbered and answering the inquiry", probe_midi, engine},
    {"quit", "SIGTERM becomes QCoreApplication::quit, and the splash client", probe_quit, engine},
};
#define NPROBES (sizeof(PROBES) / sizeof(PROBES[0]))

/* --- results --------------------------------------------------------------- */

static int count;
static const char *current = "startup";

static void line(const char *status, const char *name, const char *suffix,
                 const char *fmt, va_list ap) {
    char detail[1024];
    vsnprintf(detail, sizeof(detail), fmt, ap);
    /* TAP gives '#' a meaning, and one line is one result. */
    for (char *c = detail; *c; c++)
        if (*c == '#' || *c == '\n') *c = ' ';
    count++;
    printf("%s %s%s%s%s\n", status, name, suffix[0] ? " " : (detail[0] ? " - " : ""),
           suffix, detail);
    fflush(stdout);
}

void t_ok(const char *name, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    line("ok", name, "", fmt, ap);
    va_end(ap);
}

void t_fail(const char *name, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    line("not ok", name, "", fmt, ap);
    va_end(ap);
}

void t_skip(const char *name, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    line("ok", name, "# SKIP ", fmt, ap);
    va_end(ap);
}

void t_note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("# ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* A probe that crashes is a result too, and must not hide the ones before it. */
static void on_crash(int sig) {
    char msg[160];
    int n = snprintf(msg, sizeof(msg), "not ok %s.crash - died with signal %d\n", current, sig);
    if (write(1, msg, (size_t)n) < 0) { /* nowhere left to say so */ }
    _exit(3);
}

/* --- what is preloaded, and what the real application imports ------------- */

struct shim shims[16];
int nshims;
static struct symset app_imported, app_needed;
static int app_known;

void *entry(const char *sym) { return dlsym(RTLD_DEFAULT, sym); }

const char *shim_exporting(const char *sym) {
    for (int i = 0; i < nshims; i++)
        if (set_has(&shims[i].def, sym)) {
            const char *base = strrchr(shims[i].path, '/');
            return base ? base + 1 : shims[i].path;
        }
    return NULL;
}

int app_imports(const char *sym) { return app_known ? set_has(&app_imported, sym) : -1; }

int app_needs(const char *lib_substring) {
    for (size_t i = 0; i < app_needed.n; i++)
        if (strstr(app_needed.v[i], lib_substring)) return 1;
    return 0;
}

static void read_preloads(void) {
    const char *env = getenv("LD_PRELOAD");
    if (!env) return;
    char *copy = strdup(env), *save = NULL;
    for (char *p = strtok_r(copy, ": ", &save); p && nshims < 16; p = strtok_r(NULL, ": ", &save)) {
        struct shim *s = &shims[nshims++];
        snprintf(s->path, sizeof(s->path), "%s", p);
        s->readable = elf_scan(p, &s->undef, NULL, &s->def, NULL, NULL, 0) == 0;
    }
    free(copy);
}

/* The real application's imports, plus those of the libraries that open files
 * on its behalf: libstdc++ for std::filesystem, libaz0x-info for the product
 * code on MPC. The libraries are found by asking the guest's loader to list
 * them, which runs none of the application. */
static void read_real(void) {
    char interp[256];
    if (!cfg.real) return;
    if (elf_scan(cfg.real, &app_imported, &app_imported, NULL, &app_needed, interp,
                 sizeof(interp)) < 0) {
        t_note("%s is not an ELF file; treating every entry point as imported", cfg.real);
        return;
    }
    app_known = 1;
    if (!interp[0]) return;
    char cmd[1024], row[1024];
    snprintf(cmd, sizeof(cmd), "LD_PRELOAD= '%s' --list '%s' 2>/dev/null", interp, cfg.real);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    while (fgets(row, sizeof(row), p)) {
        char *path = strstr(row, "=> /");
        if (!path) continue;
        path += 3;
        path[strcspn(path, " \n")] = '\0';
        if (strstr(path, "libstdc++") || strstr(path, "libaz0x"))
            elf_scan(path, &app_imported, &app_imported, NULL, NULL, NULL, 0);
    }
    pclose(p);
}

/* --- main ------------------------------------------------------------------ */

static int wanted(const char *list, const char *name) {
    if (!list) return 1;
    size_t n = strlen(name);
    for (const char *p = list; *p; p += strcspn(p, ","), p += *p == ',')
        if (strncmp(p, name, n) == 0 && (p[n] == ',' || p[n] == '\0')) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--list")) {
            for (size_t p = 0; p < NPROBES; p++)
                printf("%-11s %s%s\n", PROBES[p].name, PROBES[p].what,
                       PROBES[p].applies == engine ? " (Engine)"
                       : PROBES[p].applies == engine_or_rk3566 ? " (Engine, RK3566)" : "");
            return 0;
        } else if (!strcmp(a, "--app") && v) { cfg.app = v; i++; }
        else if (!strcmp(a, "--soc") && v) { cfg.soc = v; i++; }
        else if (!strcmp(a, "--code") && v) { cfg.code = *v ? v : NULL; i++; }
        else if (!strcmp(a, "--real") && v) { cfg.real = *v ? v : NULL; i++; }
        else if (!strcmp(a, "--probes") && v) { only = *v ? v : NULL; i++; }
        else {
            fprintf(stderr, "usage: Engine [--app engine|mpc|...] [--soc rk3588|rk3566|rk3288]\n"
                            "              [--code CODE] [--real /path/to/application]\n"
                            "              [--probes a,b,...] | --list\n");
            return 2;
        }
    }

    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGFPE, on_crash);

    read_preloads();
    read_real();
    t_note("fake Engine: app %s, soc %s, code %s, standing in for %s", cfg.app,
           cfg.soc[0] ? cfg.soc : "?", cfg.code ? cfg.code : "?",
           cfg.real ? cfg.real : "nothing in particular");

    for (size_t p = 0; p < NPROBES; p++) {
        if (!wanted(only, PROBES[p].name)) continue;
        if (!only && !PROBES[p].applies()) continue;
        current = PROBES[p].name;
        PROBES[p].run();
    }
    printf("1..%d\n", count);
    return 0;
}
