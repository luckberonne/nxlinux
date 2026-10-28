// A guest module (NSO/NRO or a test shared object) mapped in the guest window.
#pragma once
#include <elf.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char name[32];
    uint64_t base;               // guest address of the image (== host address)
    size_t size;                 // image + bss, page-rounded
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint64_t init, init_array, init_arraysz;
} module_t;

// img: text+ro+data laid out at their memory offsets (what tools/nso.py produces); bss zero bytes follow it.
// dyn_off: offset of the dynamic section from base (from MOD0 for an NSO: module_dyn_from_mod0).
int module_map(module_t *m, const char *name, const uint8_t *img, size_t len, size_t bss, uint64_t dyn_off);
int64_t module_dyn_from_mod0(const uint8_t *img, size_t len);   // -1 if there is no MOD0
// Relocates m. Undefined symbols bind to the first loaded module that defines them, else to an HLE stub.
// Returns the number of imports that bound to a stub with no registered host function.
int module_relocate(module_t *m, module_t **loaded, int nloaded);
uint64_t module_sym(const module_t *m, const char *name);   // guest address of a defined symbol, 0 if none
