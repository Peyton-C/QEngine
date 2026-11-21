/* Just enough ELF to list what an object imports, exports and links against.
 *
 * The fake reads the real application and the preloaded shims as data with
 * this, so it can tell which libc entry points matter on the firmware it is
 * running in. It runs in the guest, where there is no readelf. Both classes are
 * handled because one source builds for armhf and arm64. */
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fake.h"

void set_add(struct symset *s, const char *name) {
    if (!s || set_has(s, name)) return;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->v = realloc(s->v, s->cap * sizeof(*s->v));
    }
    s->v[s->n++] = strdup(name);
}

int set_has(const struct symset *s, const char *name) {
    if (!s) return 0;
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->v[i], name) == 0) return 1;
    return 0;
}

/* A string at off, or NULL if it is not inside the file and terminated there. */
static const char *str_at(const unsigned char *b, size_t len, size_t off) {
    if (off >= len || !memchr(b + off, 0, len - off)) return NULL;
    return (const char *)b + off;
}

#define SCAN(BITS)                                                                    \
    static void scan##BITS(const unsigned char *b, size_t len, struct symset *undef,  \
                           struct symset *weak, struct symset *def,                   \
                           struct symset *needed, char *interp, size_t interp_len) {  \
        const Elf##BITS##_Ehdr *eh = (const void *)b;                                 \
        if (len < sizeof(*eh)) return;                                                \
        const Elf##BITS##_Phdr *ph = (const void *)(b + eh->e_phoff);                 \
        if (interp && eh->e_phoff && eh->e_phoff < len &&                             \
            (len - eh->e_phoff) / sizeof(*ph) >= eh->e_phnum)                         \
            for (unsigned i = 0; i < eh->e_phnum; i++)                                \
                if (ph[i].p_type == PT_INTERP) {                                      \
                    const char *s = str_at(b, len, ph[i].p_offset);                   \
                    if (s && strlen(s) < interp_len) strcpy(interp, s);               \
                }                                                                     \
        const Elf##BITS##_Shdr *sh = (const void *)(b + eh->e_shoff);                 \
        if (!eh->e_shoff || eh->e_shoff >= len ||                                     \
            (len - eh->e_shoff) / sizeof(*sh) < eh->e_shnum)                          \
            return;                                                                   \
        for (unsigned i = 0; i < eh->e_shnum; i++) {                                  \
            if (sh[i].sh_link >= eh->e_shnum || sh[i].sh_offset >= len ||             \
                sh[i].sh_size > len - sh[i].sh_offset)                                \
                continue;                                                             \
            size_t strtab = sh[sh[i].sh_link].sh_offset;                              \
            if (sh[i].sh_type == SHT_DYNSYM) {                                        \
                const Elf##BITS##_Sym *sym = (const void *)(b + sh[i].sh_offset);     \
                size_t count = sh[i].sh_size / sizeof(*sym);                          \
                for (size_t j = 1; j < count; j++) {                                  \
                    const char *name = str_at(b, len, strtab + sym[j].st_name);       \
                    int bind = ELF##BITS##_ST_BIND(sym[j].st_info);                   \
                    if (!name || !*name) continue;                                    \
                    if (sym[j].st_shndx == SHN_UNDEF)                                 \
                        set_add(bind == STB_WEAK ? weak : undef, name);               \
                    else if (bind == STB_GLOBAL || bind == STB_WEAK)                  \
                        set_add(def, name);                                           \
                }                                                                     \
            } else if (sh[i].sh_type == SHT_DYNAMIC && needed) {                      \
                const Elf##BITS##_Dyn *dyn = (const void *)(b + sh[i].sh_offset);     \
                size_t count = sh[i].sh_size / sizeof(*dyn);                          \
                for (size_t j = 0; j < count && dyn[j].d_tag != DT_NULL; j++)         \
                    if (dyn[j].d_tag == DT_NEEDED) {                                  \
                        const char *name = str_at(b, len, strtab + dyn[j].d_un.d_val);\
                        if (name) set_add(needed, name);                              \
                    }                                                                 \
            }                                                                         \
        }                                                                             \
    }

SCAN(32)
SCAN(64)

int elf_scan(const char *path, struct symset *undef, struct symset *weak_undef,
             struct symset *def, struct symset *needed, char *interp, size_t interp_len) {
    if (interp && interp_len) interp[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)EI_NIDENT) {
        close(fd);
        return -1;
    }
    size_t len = (size_t)st.st_size;
    const unsigned char *b = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (b == MAP_FAILED) return -1;
    int ok = memcmp(b, ELFMAG, SELFMAG) == 0 && b[EI_DATA] == ELFDATA2LSB;
    if (ok && b[EI_CLASS] == ELFCLASS64)
        scan64(b, len, undef, weak_undef, def, needed, interp, interp_len);
    else if (ok && b[EI_CLASS] == ELFCLASS32)
        scan32(b, len, undef, weak_undef, def, needed, interp, interp_len);
    munmap((void *)b, len);
    return ok ? 0 : -1;
}
