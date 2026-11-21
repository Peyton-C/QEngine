/* The libc entry points a file access can go through, and the rule for judging
 * each. See WHICH LIBC ENTRY POINTS in main.c.
 *
 * Every call here is made through a pointer from entry(), with the prototype
 * written out by hand, never through this program's own headers: which symbol
 * a plain stat() becomes depends on the compiler flags, and the point is to
 * reach each symbol whatever this was built with. Stat buffers are opaque for
 * the same reason. Only the return value is looked at, and the buffer is
 * larger than any layout of struct stat or statx. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fake.h"

/* _STAT_VER, which the pre-2.33 __xstat family takes first. */
#ifdef __aarch64__
#define FAKE_STAT_VER 0
#else
#define FAKE_STAT_VER 3
#endif

static long slurp_fd(int fd, char *buf, size_t n) {
    if (fd < 0) return -1;
    size_t got = 0;
    for (;;) {
        ssize_t r = read(fd, buf + got, n - 1 - got);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { int e = errno; close(fd); errno = e; return -1; }
        if (r == 0 || (got += (size_t)r) == n - 1) break;
    }
    close(fd);
    buf[got] = '\0';
    return (long)got;
}

static long v_open(void *fn, const char *p, char *b, size_t n) {
    return slurp_fd(((int (*)(const char *, int, ...))fn)(p, O_RDONLY), b, n);
}
/* What _FORTIFY_SOURCE turns an open() with computed flags into. */
static long v_open_2(void *fn, const char *p, char *b, size_t n) {
    return slurp_fd(((int (*)(const char *, int))fn)(p, O_RDONLY), b, n);
}
static long v_openat(void *fn, const char *p, char *b, size_t n) {
    return slurp_fd(((int (*)(int, const char *, int, ...))fn)(AT_FDCWD, p, O_RDONLY), b, n);
}
static long v_openat_2(void *fn, const char *p, char *b, size_t n) {
    return slurp_fd(((int (*)(int, const char *, int))fn)(AT_FDCWD, p, O_RDONLY), b, n);
}
static long v_fopen(void *fn, const char *p, char *b, size_t n) {
    FILE *f = ((FILE *(*)(const char *, const char *))fn)(p, "r");
    if (!f) return -1;
    size_t got = fread(b, 1, n - 1, f);
    fclose(f);
    b[got] = '\0';
    return (long)got;
}

const struct variant READ_VARIANTS[] = {
    {"open", v_open},           {"open64", v_open},
    {"__open_2", v_open_2},     {"__open64_2", v_open_2},
    {"openat", v_openat},       {"openat64", v_openat},
    {"__openat_2", v_openat_2}, {"__openat64_2", v_openat_2},
    {"fopen", v_fopen},         {"fopen64", v_fopen},
    {NULL, NULL},
};

static long long stat_buf[512];

static long v_stat(void *fn, const char *p, char *b, size_t n) {
    (void)b; (void)n;
    return ((int (*)(const char *, void *))fn)(p, stat_buf);
}
static long v_fstatat(void *fn, const char *p, char *b, size_t n) {
    (void)b; (void)n;
    return ((int (*)(int, const char *, void *, int))fn)(AT_FDCWD, p, stat_buf, 0);
}
static long v_xstat(void *fn, const char *p, char *b, size_t n) {
    (void)b; (void)n;
    return ((int (*)(int, const char *, void *))fn)(FAKE_STAT_VER, p, stat_buf);
}
static long v_statx(void *fn, const char *p, char *b, size_t n) {
    (void)b; (void)n;
    return ((int (*)(int, const char *, int, unsigned, void *))fn)(AT_FDCWD, p, 0, 0x7ff,
                                                                  stat_buf);
}

const struct variant STAT_VARIANTS[] = {
    {"stat", v_stat},                     {"lstat", v_stat},
    {"stat64", v_stat},                   {"lstat64", v_stat},
    {"__stat64_time64", v_stat},          {"__lstat64_time64", v_stat},
    {"fstatat", v_fstatat},               {"fstatat64", v_fstatat},
    {"__fstatat64_time64", v_fstatat},    {"__xstat", v_xstat},
    {"__lxstat", v_xstat},                {"__xstat64", v_xstat},
    {"__lxstat64", v_xstat},              {"statx", v_statx},
    {NULL, NULL},
};

static long v_access(void *fn, const char *p, char *b, size_t n) {
    (void)b; (void)n;
    return ((int (*)(const char *, int))fn)(p, R_OK);
}
static long v_faccessat(void *fn, const char *p, char *b, size_t n) {
    (void)b; (void)n;
    return ((int (*)(int, const char *, int, int))fn)(AT_FDCWD, p, R_OK, 0);
}

const struct variant ACCESS_VARIANTS[] = {
    {"access", v_access}, {"faccessat", v_faccessat}, {NULL, NULL},
};

static long v_realpath(void *fn, const char *p, char *b, size_t n) {
    char out[PATH_MAX];
    if (!((char *(*)(const char *, char *))fn)(p, out)) return -1;
    snprintf(b, n, "%s", out);
    return (long)strlen(b);
}
static long v_realpath_chk(void *fn, const char *p, char *b, size_t n) {
    char out[PATH_MAX];
    if (!((char *(*)(const char *, char *, size_t))fn)(p, out, sizeof(out))) return -1;
    snprintf(b, n, "%s", out);
    return (long)strlen(b);
}

const struct variant PATH_VARIANTS[] = {
    {"realpath", v_realpath}, {"__realpath_chk", v_realpath_chk}, {NULL, NULL},
};

/* Whether libc itself has this entry point. A shim's wrapper alone does not
 * count: the legacy dtshim exports __stat64_time64 on a glibc that has none,
 * where nothing can import it and the wrapper has no real function to call. */
static int libc_has(const char *sym) {
    static void *libc;
    if (!libc) libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
    return libc && dlsym(libc, sym) != NULL;
}

int run_family(const char *name, const struct variant *variants, const char *path,
               check_fn check, void *arg) {
    int good = 0, callers = 0;
    char via[256] = "", line_name[160];
    static char buf[1 << 17]; /* /proc/interrupts runs to tens of kilobytes */
    for (const struct variant *v = variants; v->sym; v++) {
        void *fn = entry(v->sym);
        if (!fn || !libc_has(v->sym)) continue; /* nothing here can be calling it */
        char detail[200] = "";
        buf[0] = '\0';
        errno = 0;
        long n = v->call(fn, path, buf, sizeof(buf));
        int worked = n >= 0;
        if (!worked) snprintf(detail, sizeof(detail), "%s", strerror(errno));
        else if (check) worked = check(buf, n, detail, sizeof(detail), arg);

        const char *shim = shim_exporting(v->sym);
        int imported = app_imports(v->sym);
        snprintf(line_name, sizeof(line_name), "%s.%s", name, v->sym);
        callers += imported != 0;
        if (worked) {
            t_ok(line_name, "%s", detail);
            if (imported != 0) {
                good++;
                snprintf(via + strlen(via), sizeof(via) - strlen(via), "%s%s",
                         via[0] ? " " : "", v->sym);
            }
        } else {
            /* Not a failure by itself, see WHICH LIBC ENTRY POINTS in main.c:
             * the application imports this for something, and nothing says it
             * is for this path. The family line below, and the baseline, judge. */
            t_skip(line_name, "%s; %s interposes it, the application %s it", detail,
                   shim ? shim : "no shim", imported == 0 ? "does not import" : "imports");
        }
    }
    if (good) t_ok(name, "through %s", via);
    else if (!callers) t_skip(name, "the application imports none of these entry points");
    else t_fail(name, "%s: no entry point the application imports gets it", path);
    return good;
}

/* Exactly this text. Identity strings carry no trailing newline, and Engine's
 * readers do not strip one. */
int check_text(const char *buf, long n, char *detail, size_t dl, void *expected) {
    const char *want = expected;
    if (n == 0) { snprintf(detail, dl, "empty"); return 0; }
    if (buf[n - 1] == '\n') { snprintf(detail, dl, "ends in a newline"); return 0; }
    if (want && strcmp(buf, want) != 0) {
        snprintf(detail, dl, "\"%.40s\", expected \"%s\"", buf, want);
        return 0;
    }
    snprintf(detail, dl, "%.60s", buf);
    return 1;
}

/* A devicetree <u32> cell holding 0: four zero bytes, not the text "0". */
int check_zero_cell(const char *buf, long n, char *detail, size_t dl, void *unused) {
    (void)unused;
    if (n != 4 || buf[0] || buf[1] || buf[2] || buf[3]) {
        snprintf(detail, dl, "%ld bytes, not a zero u32 cell", n);
        return 0;
    }
    snprintf(detail, dl, "<0>");
    return 1;
}

/* A sysfs attribute: the text and one newline. */
int check_line(const char *buf, long n, char *detail, size_t dl, void *expected) {
    const char *want = expected;
    size_t len = strlen(want);
    if ((size_t)n != len + 1 || strncmp(buf, want, len) != 0 || buf[len] != '\n') {
        snprintf(detail, dl, "\"%.40s\", expected \"%s\"", buf, want);
        return 0;
    }
    snprintf(detail, dl, "%s", want);
    return 1;
}
