// HLE imports: the host functions a guest module may call by name. Each import resolves to a stub in the guest window
// (`svc #(HLE_SVC_BASE+i); ret`); the svc handler calls the registered C function with the guest's argument registers.
#pragma once
#include <stdint.h>
#include "cpu.h"

#define HLE_SVC_BASE 0x1000u

void hle_register(const char *name, void *fn);
void hle_register_force(const char *name, void *fn);  // as hle_register, and the loader binds every reference to name to it, even where a loaded module defines it (HLE_FORCE)
int hle_is_forced(const char *name);        // fn: C function with an AAPCS64-compatible signature (see hle_call.h)
uint64_t hle_stub_for(const char *name);              // guest address of the stub for name; an unregistered name gets a logging stub
int hle_implements(const char *name);
const char *hle_stub_name(uint32_t swi);              // the name a stub svc number stands for, NULL if none
// Returns 1 if swi was an HLE stub call and has been handled.
int hle_dispatch(cpu_t *c, uint32_t swi);
extern unsigned long hle_unimplemented_calls;
