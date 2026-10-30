#include "module32.h"
#include "guest_mem.h"
#include "hle_stub.h"
#include "svc32.h"
#include <stdio.h>
#include <string.h>

#ifndef DT_RELR
#define DT_RELR 36
#define DT_RELRSZ 35
#endif

static module32_t *g_mods32[64]; static int g_nmods32;

int module32_map(module32_t *m, module_t *base, const char *name, const uint8_t *img, size_t len, size_t bss, uint64_t dyn_off, uint64_t ro_off, uint64_t data_off) {
    memset(m, 0, sizeof *m);
    memset(base, 0, sizeof *base);
    m->m = base;
    snprintf(base->name, sizeof base->name, "%s", name);
    base->size = (len + bss + 0xFFF) & ~(size_t)0xFFF;
    base->base = guest_alloc(base->size, 0x1000);              // back to back: rtld finds the modules by walking QueryMemory over them
    if (!base->base) { fprintf(stderr, "[RED] %s: guest window full\n", name); return -1; }
    if (base->base + base->size > 0x100000000ull) { fprintf(stderr, "[RED] %s: does not fit below 4 GiB\n", name); return -1; }
    memcpy((void *)base->base, img, len);                 // the window is zero-filled, so bss needs nothing
    base->ro_off = ro_off; base->data_off = data_off;
    if (dyn_off + sizeof(Elf32_Dyn) > len) { fprintf(stderr, "[RED] %s: dynamic section outside the image\n", name); return -1; }
    m->dyn = (const Elf32_Dyn *)(base->base + dyn_off);
    for (const Elf32_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf32_Sym *)(base->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(base->base + d->d_un.d_ptr); break;
    }
    if (m->symtab && m->strtab && (const uint8_t *)m->strtab > (const uint8_t *)m->symtab)   // the string table follows the symbols, as the linker lays them out
        m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf32_Sym));
    if (g_nmods32 < 64) g_mods32[g_nmods32++] = m;
    return 0;
}

uint32_t module32_sym(const module32_t *m, const char *name);
uint32_t module32_find(const char *name) {
    for (int i = 0; i < g_nmods32; i++) { uint32_t a = module32_sym(g_mods32[i], name); if (a) return a; }
    return 0;
}

uint32_t module32_sym(const module32_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf32_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return (uint32_t)m->m->base + s->st_value;
    }
    return 0;
}

// The import stub of hle_stub.c is AArch64 (`svc #n; ret`); an AArch32 guest needs `svc #n; bx lr`, with the same n so hle_dispatch finds it.
static uint32_t stub32_for(const char *name) {
    uint64_t a = hle_stub_for(name);
    if (!a) return 0;
    uint32_t *code = (uint32_t *)a;
    if ((code[0] & 0xFFE0001Fu) == 0xD4000001u) {          // still the A64 encoding
        uint32_t swi = (code[0] >> 5) & 0xFFFF;
        code[0] = 0xEF000000u | swi;
        code[1] = 0xE12FFF1Eu;                              // bx lr
    }
    return (uint32_t)a;
}

static uint32_t bind(const module32_t *m, const Elf32_Sym *s, module32_t **loaded, int nloaded, int *unimpl) {
    const char *nm = m->strtab + s->st_name;
    if (hle_is_forced(nm)) return stub32_for(nm);
    for (int i = 0; i < nloaded; i++) { uint32_t a = module32_sym(loaded[i], nm); if (a) return a; }
    if (s->st_shndx) return (uint32_t)m->m->base + s->st_value;
    if (!hle_implements(nm) && ELF32_ST_BIND(s->st_info) == STB_WEAK) return 0;
    if (!hle_implements(nm)) (*unimpl)++;
    return stub32_for(nm);
}

static unsigned relr_apply(const module32_t *m, uint32_t relr, uint32_t relrsz) {
    const uint32_t *e = (const uint32_t *)(m->m->base + relr);
    uint32_t *where = NULL; unsigned n = 0;
    for (uint32_t i = 0; i < relrsz / 4; i++) {
        uint32_t v = e[i];
        if (!(v & 1)) { where = (uint32_t *)(m->m->base + v); *where++ += (uint32_t)m->m->base; n++; continue; }
        if (!where) return n;
        for (unsigned b = 0; (v >>= 1); b++) if (v & 1) { where[b] += (uint32_t)m->m->base; n++; }
        where += 31;
    }
    return n;
}

static int apply(const module32_t *m, uint32_t off, uint32_t info, int has_addend, int32_t addend, module32_t **loaded, int nloaded, int *unimpl) {
    uint32_t *where = (uint32_t *)(m->m->base + off);
    uint32_t base = (uint32_t)m->m->base, type = ELF32_R_TYPE(info), sym = ELF32_R_SYM(info);
    uint32_t a = has_addend ? (uint32_t)addend : *where;      // REL keeps the addend in the place itself
    if (type == R_ARM_NONE) return 0;
    if (type == R_ARM_RELATIVE) { *where = base + a; return 0; }
    if (!m->symtab) { fprintf(stderr, "[RED] %s: symbol relocation without a dynamic symbol table\n", m->m->name); return -1; }
    uint32_t v = sym ? bind(m, &m->symtab[sym], loaded, nloaded, unimpl) : 0;
    switch (type) {
    case R_ARM_JUMP_SLOT: case R_ARM_GLOB_DAT: *where = v; return 0;
    case R_ARM_ABS32: *where = v + a; return 0;
    default: fprintf(stderr, "[RED] %s: unsupported relocation type %u at +%#x\n", m->m->name, type, off); return 0;
    }
}

int module32_relocate(module32_t *m, module32_t **loaded, int nloaded) {
    uint32_t rel = 0, relsz = 0, rela = 0, relasz = 0, jmprel = 0, pltsz = 0, pltrel = DT_REL, relr = 0, relrsz = 0;
    for (const Elf32_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_REL: rel = d->d_un.d_ptr; break;           case DT_RELSZ: relsz = d->d_un.d_val; break;
        case DT_RELA: rela = d->d_un.d_ptr; break;         case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;     case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_PLTREL: pltrel = d->d_un.d_val; break;
        case DT_RELR: relr = d->d_un.d_ptr; break;         case DT_RELRSZ: relrsz = d->d_un.d_val; break;
    }
    int unimpl = 0;
    if (relr) relr_apply(m, relr, relrsz);
    if (rel) for (uint32_t i = 0; i < relsz / sizeof(Elf32_Rel); i++) {
        const Elf32_Rel *r = (const Elf32_Rel *)(m->m->base + rel) + i;
        if (apply(m, r->r_offset, r->r_info, 0, 0, loaded, nloaded, &unimpl)) return -1;
    }
    if (rela) for (uint32_t i = 0; i < relasz / sizeof(Elf32_Rela); i++) {
        const Elf32_Rela *r = (const Elf32_Rela *)(m->m->base + rela) + i;
        if (apply(m, r->r_offset, r->r_info, 1, r->r_addend, loaded, nloaded, &unimpl)) return -1;
    }
    if (jmprel) {
        uint32_t esz = pltrel == DT_RELA ? sizeof(Elf32_Rela) : sizeof(Elf32_Rel);
        for (uint32_t i = 0; i < pltsz / esz; i++) {
            const uint8_t *p = (const uint8_t *)(m->m->base + jmprel) + i * esz;
            const Elf32_Rel *r = (const Elf32_Rel *)p;
            int has = pltrel == DT_RELA;
            if (apply(m, r->r_offset, r->r_info, has, has ? ((const Elf32_Rela *)p)->r_addend : 0, loaded, nloaded, &unimpl)) return -1;
        }
    }
    return unimpl;
}

int module32_hook_exports(module32_t *m, int *skipped) {
    int n = 0;
    *skipped = 0;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf32_Sym *s = &m->symtab[i];
        if (!s->st_shndx || ELF32_ST_TYPE(s->st_info) != STT_FUNC) continue;
        const char *nm = m->strtab + s->st_name;
        if (!hle_implements(nm)) continue;
        // The SDK's HIPC client runs over the SVC-level IPC (ipc.c): the sf hooks, which answer a fixed list of services themselves, would cut it off
        if (!strncmp(nm, "_ZN2nn2sf4hipc", 14)) continue;
        uint32_t stub = stub32_for(nm);
        uint32_t swi = *(uint32_t *)(uintptr_t)stub & 0xFFFFFF;       // the stub's own `svc #n`: the function gets the same number
        if (s->st_value & 1) {                                        // Thumb: movw r12, #n; svc #0xF0 (a Thumb svc has 8 bits only; cpu32 reads n from r12)
            if (s->st_size < 6) { (*skipped)++; fprintf(stderr, "[RED] %s: %s is a Thumb function under 6 bytes, which cannot be taken over\n", m->m->name, nm); continue; }
            uint16_t *hw = (uint16_t *)(m->m->base + (s->st_value & ~1u));
            hw[0] = (uint16_t)(0xF240 | (swi >> 11 & 1) << 10 | (swi >> 12 & 15));
            hw[1] = (uint16_t)((swi >> 8 & 7) << 12 | 12 << 8 | (swi & 0xFF));
            hw[2] = 0xDFF0;
        } else {
            if (s->st_size < 4) { (*skipped)++; continue; }
            *(uint32_t *)(m->m->base + s->st_value) = 0xEF000000u | swi;   // svc #n; cpu32 returns to lr itself
        }
        svc32_register_hook(swi, nm);
        n++;
    }
    return n;
}
