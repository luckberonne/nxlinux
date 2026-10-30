// The guest window: one fixed mapping where guest address == host address.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define GUEST_BASE_A64 0x2000000000ull
#define GUEST_BASE_A32 0x00200000ull       // where Horizon places the first module of a 32-bit process
#define GUEST_BASE guest_mem_base()        // the window's start: GUEST_BASE_A64, or GUEST_BASE_A32 after guest_mem_init32

uint64_t guest_mem_base(void);
int guest_mem_init(size_t size);          // reserve [GUEST_BASE, GUEST_BASE+size); 0 on success
// The same window for an AArch32 process, whose pointers are 32 bits: guest address == host address still holds, so it sits in the low 4 GiB.
// size is clamped to what fits below 4 GiB.
int guest_mem_init32(size_t size);
uint64_t guest_alloc(size_t size, size_t align);   // bump allocation inside the window (module images, stub page); 0 if full
uint64_t guest_mem_end(void);
void guest_alloc_limit(uint64_t end);     // guest_alloc() hands out addresses below end (the low region, before the alias region)
size_t guest_mem_size(void);
