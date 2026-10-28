#include "guest_mem.h"
#include <sys/mman.h>
#include <stdio.h>

static size_t g_size;
static uint64_t g_next;

int guest_mem_init(size_t size) {
    void *p = mmap((void *)GUEST_BASE, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)GUEST_BASE) { perror("guest window"); return -1; }
    g_size = size;
    g_next = GUEST_BASE + 0x10000;                // the first 64 KiB stay unmapped-by-convention (null-ish guest addresses)
    return 0;
}

uint64_t guest_alloc(size_t size, size_t align) {
    uint64_t a = (g_next + align - 1) & ~(uint64_t)(align - 1);
    if (a + size > GUEST_BASE + g_size) return 0;
    g_next = a + size;
    return a;
}

uint64_t guest_mem_end(void) { return GUEST_BASE + g_size; }
