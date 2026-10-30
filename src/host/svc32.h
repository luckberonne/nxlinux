// The Horizon AArch32 SVC ABI → the AArch64 register layout the kernel and memory SVCs (kernel.c, mem.c) are written against.
// A 32-bit process passes the arguments of each SVC in r0..r7 as the kernel's wrapper generator lays them out (Atmosphère, vapours svc_codegen_impl_layout.hpp): the inputs
// keep the register the AAPCS32 call of the SVC's prototype gives them, 64-bit values take a register pair, and an input that AAPCS32 would put on the stack and every output take
// the first free registers. svc32_enter rebuilds the x0..x7 those handlers read; svc32_leave puts their x0..x3 results back into r0..r7.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t sret; } svc32_ctx_t;     // per-call state: where a hook returning a struct by memory (AAPCS32: anything over 4 bytes) must write its result

// r: the guest's r0..r15 (only r0..r7 are read). x: the handler's x0..x7, every one set (unused ones to 0).
void svc32_enter(uint32_t num, const uint32_t r[16], uint64_t x[8], svc32_ctx_t *cx);
// x: the handler's x0..x7 after it ran. r: the guest's registers, only the outputs of num are written.
void svc32_leave(uint32_t num, const uint64_t x[8], uint32_t r[16], svc32_ctx_t *cx);
// A host function that replaces a guest function (an HLE hook, stub svc number >= 0x1000): the guest calls it with AAPCS32 arguments, and svc32_enter lays them out as the AArch64 x0..x7
// the host function reads. mangled is the guest function's Itanium name, which gives the parameters (a 64-bit integer takes an even register pair; everything else is one word,
// a pointer included); a name that does not parse, or a C name, is taken as eight one-word parameters. The result comes back in r0 (and its high word in r1).
// Returns the number of parameters found, or -1 if the name did not parse.
int svc32_register_hook(uint32_t swi, const char *mangled);
// svc32_enter and svc32_leave for a title with VFP arguments: ext is the guest's s0..s63 (as dynarmic keeps them). fv[0..7] receives the float and double parameters of a hook as the host
// function reads them in v0.. (a float in the low 32 bits); fret is what the host function left in v0 (a float result is its low word, a double the whole).
void svc32_enter2(uint32_t num, const uint32_t r[16], const uint32_t *ext, uint64_t x[8], uint64_t fv[8], svc32_ctx_t *cx);
void svc32_leave2(uint32_t num, const uint64_t x[8], uint64_t fret, uint32_t r[16], uint32_t *ext, svc32_ctx_t *cx);
// A hook (stub svc number >= 0x1000) with its parameters given as a string, one letter each: i a word, x a 64-bit integer, f a float, d a double; ret is one of those or v. Returns the count.
int svc32_register_sig(uint32_t swi, const char *params, char ret);
// 1 if num has a layout in the table; others pass r0..r7 through unchanged and return r0 alone.
int svc32_known(uint32_t num);
#ifdef __cplusplus
}
#endif
