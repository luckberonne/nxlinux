// Calls a host C function with the arguments an AArch64 guest left in its registers (AAPCS64 -> SysV x86-64).
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t x[8];   // x0..x7: integer and pointer arguments; x0 also receives the result
    uint64_t d[8];   // low 64 bits of v0..v7: float (low 32 bits) and double arguments; d[0] also receives the result
} hle_regs_t;

// fn takes up to 8 integer and 8 floating-point register arguments (no variadics, no structs by value, no x8
// indirect result). The first 6 integers go in registers and the 7th and 8th on the stack, as SysV requires.
void hle_call(void *fn, hle_regs_t *r);
