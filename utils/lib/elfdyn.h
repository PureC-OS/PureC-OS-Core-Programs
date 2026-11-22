#ifndef UTILS_ELFDYN_H
#define UTILS_ELFDYN_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELFD_MAX_SIZE (96u * 1024u * 1024u)

struct elfd_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elfd_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct elfd_dyn {
    int64_t tag;
    uint64_t val;
};

struct elfd {
    uint8_t *data;
    size_t size;
    int type;
    int has_interp;
    const struct elfd_dyn *dyn;
    size_t ndyn;
    const char *strtab;
    size_t strsz;
};

static inline void elfd_free(struct elfd *e)
{
    free(e->data);
    memset(e, 0, sizeof(*e));
}

static inline int elfd_load(const char *path, struct elfd *e)
{
    memset(e, 0, sizeof(*e));
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t cap = 65536, len = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (len == cap) {
            if (cap >= ELFD_MAX_SIZE) { free(buf); buf = NULL; break; }
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        if (got == 0)
            break;
        len += got;
    }
    fclose(f);
    if (!buf)
        return -1;
    e->data = buf;
    e->size = len;
    if (len < sizeof(struct elfd_ehdr) || memcmp(buf, "\177ELF", 4) != 0 || buf[4] != 2 || buf[5] != 1) {
        elfd_free(e);
        return -2;
    }
    struct elfd_ehdr eh;
    memcpy(&eh, buf, sizeof(eh));
    if (eh.machine != 62 || eh.phentsize != sizeof(struct elfd_phdr) ||
        eh.phoff > len || (uint64_t)eh.phnum * sizeof(struct elfd_phdr) > len - eh.phoff) {
        elfd_free(e);
        return -2;
    }
    e->type = eh.type;
    const struct elfd_phdr *ph = (const struct elfd_phdr *)(void *)(buf + eh.phoff);
    uint64_t dynoff = 0, dynsz = 0;
    int have_dyn = 0;
    for (int i = 0; i < eh.phnum; i++) {
        struct elfd_phdr p;
        memcpy(&p, &ph[i], sizeof(p));
        if (p.type == 3)
            e->has_interp = 1;
        else if (p.type == 2 && p.offset <= len && p.filesz <= len - p.offset) {
            dynoff = p.offset;
            dynsz = p.filesz;
            have_dyn = 1;
        }
    }
    if (!have_dyn)
        return 0;
    uint64_t strtab = 0, strsz = 0;
    size_t n = dynsz / sizeof(struct elfd_dyn);
    const struct elfd_dyn *d = (const struct elfd_dyn *)(void *)(buf + dynoff);
    for (size_t i = 0; i < n; i++) {
        struct elfd_dyn x;
        memcpy(&x, &d[i], sizeof(x));
        if (x.tag == 0) { n = i; break; }
        if (x.tag == 5) strtab = x.val;
        else if (x.tag == 10) strsz = x.val;
    }
    e->dyn = d;
    e->ndyn = n;
    uint64_t off = 0;
    int found = 0;
    for (int i = 0; i < eh.phnum; i++) {
        struct elfd_phdr p;
        memcpy(&p, &ph[i], sizeof(p));
        if (p.type == 1 && strtab >= p.vaddr && strtab < p.vaddr + p.filesz) {
            off = p.offset + (strtab - p.vaddr);
            found = 1;
            break;
        }
    }
    if (found && off < len) {
        e->strtab = (const char *)(buf + off);
        e->strsz = strsz && strsz <= len - off ? strsz : len - off;
    }
    return 0;
}

static inline const char *elfd_str(const struct elfd *e, uint64_t off)
{
    if (!e->strtab || off >= e->strsz)
        return NULL;
    const char *s = e->strtab + off;
    if (!memchr(s, 0, e->strsz - off))
        return NULL;
    return s;
}

static inline const char *elfd_tag_str(const struct elfd *e, int64_t tag, size_t nth)
{
    for (size_t i = 0; i < e->ndyn; i++) {
        struct elfd_dyn x;
        memcpy(&x, &e->dyn[i], sizeof(x));
        if (x.tag == tag) {
            if (nth == 0)
                return elfd_str(e, x.val);
            nth--;
        }
    }
    return NULL;
}

#define ELFD_NEEDED 1
#define ELFD_SONAME 14
#define ELFD_RPATH 15
#define ELFD_RUNPATH 29

#endif
