#include "guest_mem.h"
#include "cpu32.h"
#include <sys/mman.h>
#include <stdio.h>

static size_t g_size;
static uint64_t g_next, g_limit, g_base = GUEST_BASE_A64;

uint64_t guest_mem_base(void) { return g_base; }

int guest_mem_init32(size_t size) {
    if (size > 0x100000000ull - GUEST_BASE_A32) size = 0x100000000ull - GUEST_BASE_A32;
    g_base = GUEST_BASE_A32;
    int rc = guest_mem_init(size);
    cpu32_set_window(g_base, g_base + g_size);
    g_next = g_base;                          // rtld sits exactly at 0x200000 and walks the modules after it with QueryMemory: no null guard here, below the window is unmapped anyway
    return rc;
}

int guest_mem_init(size_t size) {
    void *p = mmap((void *)g_base, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)g_base) { perror("guest window"); return -1; }
    g_size = size;
    g_limit = g_base + size;
    g_next = g_base + 0x10000;                // the first 64 KiB stay unmapped-by-convention (null-ish guest addresses)
    return 0;
}

uint64_t guest_alloc(size_t size, size_t align) {
    uint64_t a = (g_next + align - 1) & ~(uint64_t)(align - 1);
    if (a + size > g_limit) return 0;
    g_next = a + size;
    return a;
}

uint64_t guest_mem_end(void) { return g_base + g_size; }

void guest_alloc_limit(uint64_t end) { g_limit = end; }
size_t guest_mem_size(void) { return g_size; }
