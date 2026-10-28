// Horizon memory SVCs and process info: SetHeapSize, SetMemoryPermission/Attribute, MapMemory/UnmapMemory, QueryMemory,
// MapPhysicalMemory/UnmapPhysicalMemory, Break, OutputDebugString, GetInfo, GetSystemTick.
//
// The guest window is partitioned the Horizon way: [low region | alias region | stack region | heap region].
// The low region holds module images, stubs and thread-local regions (guest_alloc). The rest is PROT_NONE until the guest
// commits it, so a stray access faults instead of reading zeros.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cpu.h"

typedef struct {
    uint64_t low;          // size of the low region (module images and host-side guest structures)
    uint64_t alias;        // size of the alias region (MapPhysicalMemory)
    uint64_t stack;        // size of the stack region (MapMemory destinations)
    uint64_t heap;         // size of the heap region (SetHeapSize)
    uint64_t app_pool;     // memory an application may use, as GetInfo(TotalMemorySize) reports it
    uint64_t mem_used;     // memory GetInfo(UsedMemorySize) reports on top of the heap, e.g. the images (profile mem_used, 64 MB default)
    int map_limit;         // MapPhysicalMemory fails with LimitReached once the pool is full (profile mem_map_limit)
    uint64_t sys_resource; // system resource size the NPDM declares
    uint64_t program_id;
} mem_profile_t;

mem_profile_t mem_default_profile(void);       // small profile for tests
uint64_t mem_window_size(const mem_profile_t *p);   // what guest_mem_init() must be given
int mem_init(const mem_profile_t *p);          // partition the window and revoke access outside the low region; 0 on success

// Handles one SVC. Returns 0 if swi is not a memory/info SVC. x0..x7 come from the cpu, results go back into x0..x3.
int mem_svc(cpu_t *c, uint32_t swi);

// Break (SVC 0x26) is fatal unless the reason has bit 31 set (a notification). The default handler prints and exits with 42.
typedef void (*mem_break_fn)(uint64_t reason, uint64_t ptr, uint64_t size, uint64_t lr);
void mem_set_break_handler(mem_break_fn fn);

// Stacks the host built for guest threads outside the stack region (the main thread's): QueryMemory reports them as Stack.
void mem_register_stack(uint64_t base, uint64_t size);

// Address ranges, for the loader, the kernel and tests.
uint64_t mem_alias_base(void);  uint64_t mem_alias_size(void);
uint64_t mem_stack_base(void);  uint64_t mem_stack_size(void);
uint64_t mem_heap_base(void);   uint64_t mem_heap_size(void);   // size = bytes currently committed
