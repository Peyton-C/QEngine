/* Shared by the fake Engine's probes. See main.c for what the program is. */
#ifndef FAKE_H
#define FAKE_H
#include <stddef.h>

struct config {
    const char *app;   /* "engine", or anything else for the probes every app shares */
    const char *soc;   /* rk3588, rk3566 or rk3288: which dtshim table to expect */
    const char *code;  /* product code the instance was built as, or NULL */
    const char *real;  /* the application this stands in for, read as data, or NULL */
};
extern struct config cfg;

/* One result line each, in TAP: "ok NAME - detail", "not ok ...", "ok ... # SKIP". */
void t_ok(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void t_fail(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void t_skip(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void t_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A set of symbol or library names read out of an ELF file. */
struct symset {
    char **v;
    size_t n, cap;
};
void set_add(struct symset *s, const char *name);
int set_has(const struct symset *s, const char *name);

/* Reads a shared object's or executable's dynamic symbols. Any of the sets may be
 * NULL. interp, if given, receives PT_INTERP. Returns 0, or -1 if it is not ELF. */
int elf_scan(const char *path, struct symset *undef, struct symset *weak_undef,
             struct symset *def, struct symset *needed, char *interp, size_t interp_len);

/* The preloaded shims, from LD_PRELOAD. */
struct shim {
    char path[256];
    struct symset def, undef;
    int readable;
};
extern struct shim shims[];
extern int nshims;

/* The libc or library entry point a call from the application would reach: the
 * first definition in the global scope, so a preloaded shim's before libc's. */
void *entry(const char *sym);
/* Basename of the shim that defines sym, or NULL. */
const char *shim_exporting(const char *sym);
/* Whether the real application imports sym: 1, 0, or -1 when there is no --real. */
int app_imports(const char *sym);
int app_needs(const char *lib_substring);

/* One way of making a call, by the symbol it goes through. call() returns the
 * number of bytes it put in buf, 0 for a call that reads nothing, or -1 with
 * errno set. */
struct variant {
    const char *sym;
    long (*call)(void *fn, const char *path, char *buf, size_t n);
};
typedef int (*check_fn)(const char *buf, long n, char *detail, size_t detail_len, void *arg);

/* Tries every variant that exists in this libc against path, one result line
 * each, then one line for the family under name. The family passes when at
 * least one variant the application imports works, and that line is the one
 * that can fail. A variant that does not work is reported as skipped, with who
 * interposes and imports it. Returns how many imported variants worked. */
int run_family(const char *name, const struct variant *variants, const char *path,
               check_fn check, void *arg);

extern const struct variant READ_VARIANTS[];   /* open, open64, fopen, ... */
extern const struct variant STAT_VARIANTS[];   /* stat64, __stat64_time64, statx, ... */
extern const struct variant ACCESS_VARIANTS[]; /* access, faccessat */
extern const struct variant PATH_VARIANTS[];   /* realpath, __realpath_chk */

/* check_fn helpers. */
int check_text(const char *buf, long n, char *detail, size_t dl, void *expected);
int check_zero_cell(const char *buf, long n, char *detail, size_t dl, void *unused);
int check_line(const char *buf, long n, char *detail, size_t dl, void *expected);

struct probe {
    const char *name;
    const char *what;
    void (*run)(void);
    int (*applies)(void); /* whether it runs when no probes are named */
};

void probe_loader(void);
void probe_surface(void);
void probe_identity(void);
void probe_devicetree(void);
void probe_devmem(void);
void probe_interrupts(void);
void probe_efuse(void);
void probe_battery(void);
void probe_tee(void);
void probe_alsa(void);
void probe_midi(void);
void probe_quit(void);

#endif
