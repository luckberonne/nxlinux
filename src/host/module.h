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
    uint64_t ro_off, data_off;   // segment offsets from base (0 = unknown): code [0,ro) RX, read-only [ro,data) R, data and bss [data,size) RW
} module_t;

// img: text+ro+data laid out at their memory offsets (what tools/nso.py produces); bss zero bytes follow it.
// dyn_off: offset of the dynamic section from base (from MOD0 for an NSO: module_dyn_from_mod0).
int module_map(module_t *m, const char *name, const uint8_t *img, size_t len, size_t bss, uint64_t dyn_off);
int64_t module_dyn_from_mod0(const uint8_t *img, size_t len);   // -1 if there is no MOD0
// Relocates m. Undefined symbols bind to the first loaded module that defines them, else to an HLE stub.
// Returns the number of imports that bound to a stub with no registered host function.
int module_relocate(module_t *m, module_t **loaded, int nloaded);
int module_hook_exports(module_t *m, int *skipped);   // see module.c: the host's functions replace the guest's by name (retail titles, relocated by their own rtld)
uint64_t module_find(const char *name);   // a defined symbol of any registered module, 0 if none
uint64_t module_sym(const module_t *m, const char *name);   // guest address of a defined symbol, 0 if none

// Registry for QueryMemory: modules are found by address.
void module_register(module_t *m);
module_t *module_of(uint64_t addr);
uint64_t module_next_base(uint64_t addr);   // the lowest module base above addr, or 0
uint64_t module_image_bytes(void);   // total size of the registered modules, page-rounded (counted against the application pool by SetHeapSize)
