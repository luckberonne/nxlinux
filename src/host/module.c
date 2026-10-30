#include "module.h"
#include "guest_mem.h"
#include "hle_stub.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
    m->base = guest_alloc(m->size, 0x1000);       // page aligned and back to back: an AArch64 rtld finds the modules by walking QueryMemory over them
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
    if (!m->symtab || !m->strtab) return 0;                // retail rtld has no dynamic symbols: it exports and imports nothing
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
    if (hle_is_forced(nm)) return hle_stub_for(nm);                // a forced takeover wins over any definition
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
            if (!m->symtab) { fprintf(stderr, "[RED] %s: symbol relocation without a dynamic symbol table\n", m->name); return -1; }
            uint64_t v = bind(m, &m->symtab[ELF64_R_SYM(r->r_info)], loaded, nloaded, &unimpl);
            if (type == R_AARCH64_JUMP_SLOT) *where = v;
            else if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_ABS64) *where = v + r->r_addend;
            else { fprintf(stderr, "[RED] %s: unsupported relocation type %u\n", m->name, type); return -1; }
        }
    }
    return unimpl;
}

#define MAX_MODULES 128
static module_t *g_modules[MAX_MODULES];
static int g_nmodules;
void module_register(module_t *m) { if (g_nmodules < MAX_MODULES) g_modules[g_nmodules++] = m; }
uint64_t module_next_base(uint64_t addr) {   // the lowest module base above addr, or 0 if none
    uint64_t best = 0;
    for (int i = 0; i < g_nmodules; i++) if (g_modules[i]->base > addr && (!best || g_modules[i]->base < best)) best = g_modules[i]->base;
    return best;
}
module_t *module_of(uint64_t addr) {
    for (int i = 0; i < g_nmodules; i++) if (addr >= g_modules[i]->base && addr - g_modules[i]->base < g_modules[i]->size) return g_modules[i];
    return NULL;
}
uint64_t module_image_bytes(void) { uint64_t n = 0; for (int i = 0; i < g_nmodules; i++) n += g_modules[i]->size; return (n + 0x1fffff) & ~0x1fffffull; }

// Retail titles are relocated by their own rtld, which would apply the relocations a second time after the host's. So the host's functions replace the guest's by name instead: the entry of every
// function the module defines and the host implements becomes `ldr x16, [pc, #8]; br x16; .quad stub` (the stub is `svc #n; ret`), and rtld, which later relocates, finds them in place.
int module_hook_exports(module_t *m, int *skipped) {
    int n = 0; *skipped = 0;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (!s->st_shndx || ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
        const char *nm = m->strtab + s->st_name;
        if (!hle_implements(nm)) continue;
        if (!strncmp(nm, "_ZN2nn2sf4hipc", 14)) continue;        // the SDK's HIPC client runs over the SVC-level IPC
        { static const char *no = (const char *)-1; if (no == (const char *)-1) no = getenv("NX_NO_HOOK");       // NX_NO_HOOK=a,b: those exports keep the guest's own code (so NX_WATCH sees what they write)
          if (no) { size_t l = strlen(nm); int skip = 0; for (const char *c = no; *c; ) { const char *e = strchr(c, ','); size_t n = e ? (size_t)(e - c) : strlen(c); if (n == l && !strncmp(c, nm, n)) skip = 1; if (!e) break; c = e + 1; } if (skip) continue; } }
        if (s->st_size < 16 && strncmp(nm, "_ZNK2nn3mii", 11) && strncmp(nm, "_ZN2nn2oe", 9)) { (*skipped)++; continue; }          // short functions are left to the guest, except the Mii validity checks (the others' host versions were never meant to replace them)
        if (s->st_size >= 4 && s->st_size < 8) {            // a lone tail call (`b target`): a branch to the stub, which sits within reach of the modules
            int64_t d = ((int64_t)hle_stub_for(nm) - (int64_t)(m->base + s->st_value)) / 4;
            if (d > -(1 << 25) && d < (1 << 25)) { *(uint32_t *)(m->base + s->st_value) = 0x14000000u | ((uint32_t)d & 0x3FFFFFF); n++; if (getenv("NX_HOOK_LIST")) fprintf(stderr, "[hook] small: %s (4 bytes)\n", nm); continue; }
        }
        if (s->st_size < 8) { (*skipped)++; fprintf(stderr, "[RED] %s: %s is a function under 8 bytes, which cannot be taken over\n", m->name, nm); continue; }
        uint32_t *fn = (uint32_t *)(m->base + s->st_value);
        uint64_t stub = hle_stub_for(nm);
        if (s->st_size < 16) { memcpy(fn, (const void *)stub, 8); n++; if (getenv("NX_HOOK_LIST")) fprintf(stderr, "[hook] small: %s (%llu bytes)\n", nm, (unsigned long long)s->st_size); continue; }          // too short for a jump: the stub's own `svc #n; ret` takes its place
        fn[0] = 0x58000050;   // ldr x16, #8
        fn[1] = 0xD61F0200;   // br x16
        memcpy(fn + 2, &stub, 8);
        n++;
    }
    return n;
}

uint64_t module_find(const char *name) {            // a defined symbol of any registered module, 0 if none
    for (int i = 0; i < g_nmodules; i++) { uint64_t a = module_sym(g_modules[i], name); if (a) return a; }
    return 0;
}

