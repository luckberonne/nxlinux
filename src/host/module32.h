// A 32-bit (AArch32) guest module mapped in the low guest window: the ELF32 counterpart of module.h. The module is registered as an ordinary module_t (QueryMemory finds it by
// address); the dynamic symbol table, which module_t holds as ELF64, is kept here.
#pragma once
#include <elf.h>
#include "module.h"

typedef struct {
    module_t *m;
    const Elf32_Dyn *dyn;
    const Elf32_Sym *symtab;
    const char *strtab;
    unsigned nsym;
} module32_t;

// Same contract as module_map. A module without a dynamic symbol table (rtld) maps with nsym == 0. ro_off and data_off (from the NSO header; 0 = unknown) split the image into the
// code / read-only / data regions QueryMemory reports.
int module32_map(module32_t *m, module_t *base, const char *name, const uint8_t *img, size_t len, size_t bss, uint64_t dyn_off, uint64_t ro_off, uint64_t data_off);
// Applies the relocations of m (REL and RELA tables, packed RELR). Undefined symbols bind to the first loaded module that defines them, else to an AArch32 HLE stub.
// Returns the number of imports that bound to a stub with no registered host function, or -1 on a malformed table.
int module32_relocate(module32_t *m, module32_t **loaded, int nloaded);
uint32_t module32_sym(const module32_t *m, const char *name);   // guest address of a defined symbol (Thumb bit included), 0 if none
// Replaces the entry of every function the module defines and the host implements (hle_implements) with `svc; bx lr` into the host function, so every caller reaches it, through
// a PLT or not. The parameters are laid out from the function's mangled name (svc32_register_hook). ARM-mode functions of 8 bytes or more only; Thumb ones are counted as skipped.
// Returns the number of functions taken over; *skipped counts the ones that could not be.
int module32_hook_exports(module32_t *m, int *skipped);
uint32_t module32_find(const char *name);   // a defined function or object of any mapped module, by name (Thumb bit included), 0 if none
