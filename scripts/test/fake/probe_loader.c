/* loader: did the guest's dynamic loader take every shim?
 *
 * A shim the loader rejects costs one line on stderr, "object ... from
 * LD_PRELOAD cannot be preloaded: ignored", and the application then runs
 * without it. That is how a shim linked against too new a glibc fails
 * (GLIBC_2.34 not found, on any firmware older than that), and it is the most
 * common way one change breaks a whole era at once.
 *
 * surface: which of each shim's exports the real application imports. Comments
 * only, never a failure: it is there to read when a probe result is surprising. */
#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "fake.h"

static int is_loaded(struct dl_phdr_info *info, size_t size, void *path) {
    (void)size;
    return info->dlpi_name && strcmp(info->dlpi_name, path) == 0;
}

void probe_loader(void) {
    if (nshims == 0) {
        t_skip("loader", "LD_PRELOAD is empty");
        return;
    }
    for (int i = 0; i < nshims; i++) {
        const struct shim *s = &shims[i];
        const char *base = strrchr(s->path, '/');
        char name[160], missing[512] = "";
        base = base ? base + 1 : s->path;
        snprintf(name, sizeof(name), "loader.%s", base);
        if (access(s->path, R_OK) != 0)
            t_fail(name, "%s is not in the rootfs", s->path);
        else if (!dl_iterate_phdr(is_loaded, (void *)s->path))
            t_fail(name, "the loader refused %s", s->path);
        else
            t_ok(name, "loaded");

        /* Lazy binding hides a missing function until the first call to it. */
        int count = 0;
        for (size_t j = 0; j < s->undef.n; j++)
            if (!entry(s->undef.v[j]) && count++ < 8)
                snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), " %s",
                         s->undef.v[j]);
        snprintf(name, sizeof(name), "loader.%s.symbols", base);
        if (!s->readable) t_skip(name, "could not read its symbol table");
        else if (count) t_fail(name, "%d unresolved:%s", count, missing);
        else t_ok(name, "%zu imports resolve", s->undef.n);
    }
}

static int internal(const char *sym) {
    static const char *const names[] = {"_init", "_fini", "_edata", "_end", "__bss_start",
                                        "__bss_start__", "__bss_end__", "_bss_end__",
                                        "__end__", NULL};
    for (int i = 0; names[i]; i++)
        if (strcmp(sym, names[i]) == 0) return 1;
    return 0;
}

void probe_surface(void) {
    if (app_imports("open") < 0) {
        t_note("surface: no --real application to compare against");
        return;
    }
    for (int i = 0; i < nshims; i++) {
        const struct shim *s = &shims[i];
        for (int used = 1; used >= 0; used--) {
            char list[2048] = "";
            for (size_t j = 0; j < s->def.n; j++) {
                const char *sym = s->def.v[j];
                if (internal(sym) || app_imports(sym) != used) continue;
                snprintf(list + strlen(list), sizeof(list) - strlen(list), " %s", sym);
            }
            if (list[0])
                t_note("surface %s, %s:%s", s->path,
                       used ? "imported by the application" : "not imported by it", list);
        }
    }
}
