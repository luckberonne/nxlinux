#include "module.h"
#include "guest_mem.h"
#include "hle_stub.h"
#include <stdio.h>
#include <string.h>

int64_t module_dyn_from_mod0(const uint8_t *img, size_t len) {
    if (len < 8) return -1;
    uint32_t mod0 = *(const uint32_t *)(img + 4);
    if ((size_t)mod0 + 8 > len || memcmp(img + mod0, "MOD0", 4)) return -1;
    return (int64_t)mod0 + *(const int32_t *)(img + mod0 + 4);
}

int module_map(module_t *m, const char *name, const uint8_t *img, size_t len, size_t bss, uint64_t dyn_off) {
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->size = (len + bss + 0xFFF) & ~(size_t)0xFFF;
    m->base = guest_alloc(m->size, 0x10000);
    if (!m->base) { fprintf(stderr, "[RED] %s: guest window full\n", name); return -1; }
    memcpy((void *)m->base, img, len);                    // the window is zero-filled, so bss needs nothing
    m->dyn = (const Elf64_Dyn *)(m->base + dyn_off);
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    if (!m->symtab || !m->strtab) { fprintf(stderr, "[RED] %s: no dynamic symbol table\n", name); return -1; }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    return 0;
}

uint64_t module_sym(const module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return 0;
}

static unsigned relr_apply(const module_t *m, uint64_t relr, uint64_t relrsz) {
    const uint64_t *e = (const uint64_t *)(m->base + relr);
    uint64_t *where = NULL; unsigned n = 0;
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        uint64_t v = e[i];
        if (!(v & 1)) { where = (uint64_t *)(m->base + v); *where++ += m->base; n++; continue; }
        if (!where) return n;
        for (unsigned b = 0; (v >>= 1); b++) if (v & 1) { where[b] += m->base; n++; }
        where += 63;
    }
    return n;
}

static uint64_t bind(const module_t *m, const Elf64_Sym *s, module_t **loaded, int nloaded, int *unimpl) {
    const char *nm = m->strtab + s->st_name;
    for (int i = 0; i < nloaded; i++) { uint64_t a = module_sym(loaded[i], nm); if (a) return a; }
    if (s->st_shndx) return m->base + s->st_value;            // defined here, nothing earlier overrides it
    if (!hle_implements(nm) && ELF64_ST_BIND(s->st_info) == STB_WEAK) return 0;   // an undefined weak symbol is 0
    if (!hle_implements(nm)) (*unimpl)++;
    return hle_stub_for(nm);
}

int module_relocate(module_t *m, module_t **loaded, int nloaded) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELR: relr = d->d_un.d_ptr; break;
        case DT_RELRSZ: relrsz = d->d_un.d_val; break;
    }
    int unimpl = 0;
    if (relr) relr_apply(m, relr, relrsz);
    const Elf64_Rela *tab[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
    uint64_t n[2] = {relasz / sizeof(Elf64_Rela), pltsz / sizeof(Elf64_Rela)};
    for (int t = 0; t < 2; t++) {
        if (!(t ? jmprel : rela)) continue;
        for (uint64_t i = 0; i < n[t]; i++) {
            const Elf64_Rela *r = &tab[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = m->base + r->r_addend; continue; }
            uint64_t v = bind(m, &m->symtab[ELF64_R_SYM(r->r_info)], loaded, nloaded, &unimpl);
            if (type == R_AARCH64_JUMP_SLOT) *where = v;
            else if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_ABS64) *where = v + r->r_addend;
            else { fprintf(stderr, "[RED] %s: unsupported relocation type %u\n", m->name, type); return -1; }
        }
    }
    return unimpl;
}
