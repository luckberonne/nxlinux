// The guest window: one fixed mapping where guest address == host address.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define GUEST_BASE 0x2000000000ull

int guest_mem_init(size_t size);          // reserve [GUEST_BASE, GUEST_BASE+size); 0 on success
uint64_t guest_alloc(size_t size, size_t align);   // bump allocation inside the window (module images, stub page); 0 if full
uint64_t guest_mem_end(void);
void guest_alloc_limit(uint64_t end);     // guest_alloc() hands out addresses below end (the low region, before the alias region)
size_t guest_mem_size(void);
