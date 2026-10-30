#include "mem.h"
#include "guest_mem.h"
#include "kernel.h"
#include "module.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

#define R_OUT_OF_MEMORY    0xD001u
#define R_LIMIT_REACHED    0x10801u
#define R_INVALID_STATE    0xD401u
#define R_INVALID_ADDRESS  0xCC01u
#define R_INVALID_SIZE     0xCA01u
#define R_INVALID_MEMREGION 0xDC01u
#define R_OUT_OF_RESOURCE  0xCE01u

static void die(const char *m) { fprintf(stderr, "[RED] mem: %s\n", m); abort(); }

static mem_profile_t g_p;
static uint8_t *g_low, *g_alias, *g_stack, *g_heap;
static size_t g_low_cap, g_alias_cap, g_stack_cap, g_heap_cap;
static size_t g_heap_cur;
static _Atomic uint64_t *g_alias_bits;      // one bit per 4 KiB page of the alias region (mapped = 1)
static _Atomic int64_t g_alias_mapped;      // bytes mapped in the alias region
static int g_inited;

// MapMemory destinations in the stack region and the sources they borrow from. One lock covers validation, protections,
// copying and the budgets; QueryMemory takes the same snapshot. map_find's index is only valid under it (map_del moves the last entry).
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

// Stacks the host made for guest threads outside the stack region (the main thread's): QueryMemory reports them as Stack.
static struct { uint64_t base, size; } g_stacks[16];
static int g_nstacks;

static mem_break_fn g_break;

mem_profile_t mem_default_profile(void) {
    mem_profile_t p = {0};
    p.low = 0x10000000; p.alias = 0x10000000; p.stack = 0x4000000; p.heap = 0x8000000;
    p.app_pool = 0x10000000; p.mem_used = 0x400000; p.map_limit = 0; p.sys_resource = 0;
    p.program_id = 0x0100152000022000ull;
    return p;
}
uint64_t mem_window_size(const mem_profile_t *p) { return p->low + p->alias + p->stack + p->heap; }

uint64_t mem_alias_base(void) { return (uint64_t)g_alias; }
uint64_t mem_alias_size(void) { return g_alias_cap; }
uint64_t mem_stack_base(void) { return (uint64_t)g_stack; }
uint64_t mem_stack_size(void) { return g_stack_cap; }
uint64_t mem_heap_base(void) { return (uint64_t)g_heap; }
uint64_t mem_heap_size(void) { return g_heap_cur; }
void mem_set_break_handler(mem_break_fn fn) { g_break = fn; }
void mem_register_stack(uint64_t base, uint64_t size) {
    pthread_mutex_lock(&g_map_lock);
    if (g_nstacks < 16) { g_stacks[g_nstacks].base = base; g_stacks[g_nstacks++].size = size; }
    pthread_mutex_unlock(&g_map_lock);
}

int mem_init(const mem_profile_t *p) {
    if (g_inited) return -1;
    if ((p->low | p->alias | p->stack | p->heap) & 0xFFF) { fprintf(stderr, "[RED] mem: region sizes must be page multiples\n"); return -1; }
    if (guest_mem_size() < mem_window_size(p)) { fprintf(stderr, "[RED] mem: the guest window is smaller than the profile needs\n"); return -1; }
    g_p = *p;
    g_low = (uint8_t *)GUEST_BASE; g_low_cap = p->low;
    g_alias = g_low + p->low;       g_alias_cap = p->alias;
    g_stack = g_alias + p->alias;   g_stack_cap = p->stack;
    g_heap = g_stack + p->stack;    g_heap_cap = p->heap;
    g_alias_bits = calloc((g_alias_cap >> 18) + 1, sizeof *g_alias_bits);
    if (!g_alias_bits) return -1;
    // Everything above the low region is inaccessible until the guest commits it
    if (mprotect(g_alias, g_alias_cap + g_stack_cap + g_heap_cap, PROT_NONE)) { perror("mem_init mprotect"); return -1; }
    guest_alloc_limit((uint64_t)g_low + p->low);
    g_inited = 1;
    return 0;
}

// ---- alias region page bitmap
static int64_t alias_pages(uint64_t off, uint64_t n, int map) {   // returns the pages whose state really changed (negative for unmap)
    int64_t ch = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        uint64_t old = map ? atomic_fetch_or(&g_alias_bits[p >> 6], m) : atomic_fetch_and(&g_alias_bits[p >> 6], ~m);
        ch += __builtin_popcountll((map ? ~old : old) & m);
        p += k;
    }
    return map ? ch : -ch;
}
static int64_t alias_unmapped(uint64_t off, uint64_t n) {          // pages of the range not mapped yet
    int64_t c = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; p++) c += !(atomic_load(&g_alias_bits[p >> 6]) >> (p & 63) & 1);
    return c;
}
static void alias_query(uint64_t addr, uint64_t *base, uint64_t *size, int *mapped) {   // the run of equal state around addr
    uint64_t p = (addr - (uint64_t)g_alias) >> 12, np = g_alias_cap >> 12, lo = p, hi = p + 1;
    #define BIT(i) ((atomic_load(&g_alias_bits[(i) >> 6]) >> ((i) & 63)) & 1)
    *mapped = (int)BIT(p);
    while (lo && (int)BIT(lo - 1) == *mapped) lo--;
    while (hi < np && (int)BIT(hi) == *mapped) hi++;
    #undef BIT
    *base = (uint64_t)g_alias + (lo << 12); *size = (hi - lo) << 12;
}

static int contains(uint64_t base, uint64_t cap, uint64_t addr, uint64_t size) { return addr >= base && size <= cap && addr - base <= cap - size; }
static int map_find(uint64_t a, uint64_t n) {            // 1-based index of a mapping overlapping [a, a+n), 0 = none; caller holds g_map_lock
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static int map_source_busy(uint64_t a, uint64_t n) {     // [a, a+n) overlaps a source borrowed by a live mapping; caller holds g_map_lock
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].src + g_map[i].size && g_map[i].src < a + n) return 1;
    return 0;
}
static int map_source_backed(uint64_t src, uint64_t size) {   // [src, src+size) is committed, writable memory the guest owns
    if (contains((uint64_t)g_heap, g_heap_cur, src, size)) return 1;
    if (contains((uint64_t)g_alias, g_alias_cap, src, size)) return !alias_unmapped(src - (uint64_t)g_alias, size);
    module_t *m = module_of(src);        // the data and bss of a module, where the SDK allocates thread stacks from in some games
    return m && m->data_off && src - m->base >= m->data_off && contains(m->base, m->size, src, size);
}
static void map_source_query(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *attr, uint64_t *perm) {
    uint64_t lo = *base, remaining = *size;
    for (int i = 0; i < g_nmap; ++i) {
        uint64_t src = g_map[i].src, end = src + g_map[i].size, lower = lo, upper = remaining;
        if (addr >= src && addr < end) {
            if (src > lower) lower = src;
            if (end - lo < upper) upper = end - lo;
            *attr = 1; *perm = 0;     // Locked / IsBorrowed
        } else if (end <= addr && end > lower) lower = end;
        else if (src > addr && src - lo < upper) upper = src - lo;
        remaining = upper - (lower - lo); lo = lower;
    }
    *base = lo; *size = remaining;
}

// ---- QueryMemory
static void query(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *state, uint64_t *perm, uint32_t *attr) {
    int k; *attr = 0;
    uint64_t ab = (uint64_t)g_alias, ae = ab + g_alias_cap;
    if (addr >= (uint64_t)g_heap && addr < (uint64_t)g_heap + g_heap_cur) { *base = (uint64_t)g_heap; *size = g_heap_cur; *state = 5; *perm = 3; }
    else if ((k = map_find(addr, 1))) { *base = g_map[k - 1].base; *size = g_map[k - 1].size; *state = g_map[k - 1].state; *perm = 3; }
    else if (addr >= ab && addr < ae) {
        int mapped; alias_query(addr, base, size, &mapped);
        *state = mapped ? 5 : 0; *perm = mapped ? 3 : 0;
    }
    else if (addr >= (uint64_t)g_stack && addr < (uint64_t)g_stack + g_stack_cap) {   // free space between mappings
        uint64_t lo = (uint64_t)g_stack, hi = lo + g_stack_cap;
        for (int i = 0; i < g_nmap; i++) {
            uint64_t e = g_map[i].base + g_map[i].size;
            if (e <= addr && e > lo) lo = e;
            if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
        }
        *base = lo; *size = hi - lo; *state = 0; *perm = 0;
    }
    else if (addr >= (uint64_t)g_heap && addr < (uint64_t)g_heap + g_heap_cap) {
        uint64_t lo = (uint64_t)g_heap + g_heap_cur;
        *base = lo; *size = (uint64_t)g_heap + g_heap_cap - lo; *state = 0; *perm = 0;
    }
    else if (module_of(addr)) {
        // Horizon's layout: code [0, ro) Code+RX, read-only [ro, data) Code+R, data and bss [data, end) CodeData+RW. The SDK's C++ exception
        // unwinding (nn::ro::detail::GetExceptionInfo) walks back over Code regions to find the module start, then reads MOD0 there.
        module_t *m = module_of(addr); uint64_t off = addr - m->base;
        uint64_t ro = m->ro_off ? m->ro_off : m->data_off ? m->data_off : m->size, da = m->data_off ? m->data_off : ro;
        if (off < ro)      { *base = m->base;        *size = ro;           *state = 3; *perm = 5; }
        else if (off < da) { *base = m->base + ro;   *size = da - ro;      *state = 3; *perm = 1; }
        else               { *base = m->base + da;   *size = m->size - da; *state = 4; *perm = 3; }
    }
    else {
        for (int i = 0; i < g_nstacks; i++)
            if (addr >= g_stacks[i].base && addr - g_stacks[i].base < g_stacks[i].size) {
                *base = g_stacks[i].base; *size = g_stacks[i].size; *state = 0xB; *perm = 3; goto done;
            }
        if (addr >= (uint64_t)g_low && addr - (uint64_t)g_low < g_low_cap) {   // host-made guest structures
            *base = addr & ~0xFFFull; *size = 0x1000; *state = 4; *perm = 3;
            // An AArch32 rtld walks every region from 0x200000 to the end of its address space: past the modules the rest of the low region is one ThreadLocal region, not CodeData
            { *size = (uint64_t)g_low + g_low_cap - *base; *state = 0xC; }      // (an AArch64 rtld does the same walk)
            uint64_t nb = module_next_base(addr); if (nb && nb - *base < *size) { *size = nb - *base; *state = 0; }       // free up to the next module
        }
        else if (cpu_is_aarch32()) {            // free space is one region up to the next mapping, or to the end of the 4 GiB address space
            uint64_t lo = (uint64_t)g_low, hi = guest_mem_end();
            if (addr < lo) { *base = 0; *size = lo; } else { *base = hi; *size = (1ull << 32) - hi; }
            *state = 0; *perm = 0;
        }
        else if (addr < (uint64_t)g_low || addr >= guest_mem_end()) {            // outside the window: one free region up to the window, or to the end of the 39-bit address space (an AArch64 rtld walks them all)
            uint64_t lo = (uint64_t)g_low, hi = guest_mem_end();
            if (addr < lo) { *base = 0; *size = lo; } else { *base = hi; *size = (1ull << 39) - hi; }
            *state = 0; *perm = 0;
        }
        else { *base = addr & ~0xFFFull; *size = guest_mem_end() - *base; *state = 0; *perm = 0; }   // in the window but in no region the host manages: free to the end of the window, no permissions
    }
done:
    map_source_query(addr, base, size, attr, perm);
}

// ---- Break
static void default_break(uint64_t reason, uint64_t ptr, uint64_t size, uint64_t lr) {
    uint32_t res = (ptr && size >= 4) ? *(uint32_t *)ptr : 0;
    fprintf(stderr, "[RED] Break reason=%#llx Result=%#x lr=%#llx\n", (unsigned long long)reason, res, (unsigned long long)lr);
    _exit(42);
}

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23, I_FreeThreadCount=24 };

static uint64_t used_memory(void) {          // caller holds g_map_lock
    uint64_t v = g_heap_cur + g_p.mem_used;
    if (g_p.map_limit) v += (uint64_t)atomic_load(&g_alias_mapped);   // with a pool limit the alias pages count too, as on Horizon
    return v;
}

int mem_svc(cpu_t *c, uint32_t num) {
    if (!g_inited) return 0;
    uint64_t regs[8];
    for (int i = 0; i < 8; i++) regs[i] = cpu_get_x(c, i);
    int handled = 1;
    switch (num) {
    case 0x01: {  // SetHeapSize(→x1 base; x1 size)
        size_t size = regs[1];
        // Larger than the heap region: ResultOutOfMemory, as KPageTable::SetHeapSize returns. nn::os::SetMemoryHeapSize turns only that, 103 and
        // LimitReached into os::ResultOutOfMemory and aborts on anything else; games find their heap size by binary search and need the error back.
        if (size > g_heap_cap) { regs[0] = R_OUT_OF_MEMORY; break; }
        if (size & 0xfff) { regs[0] = R_INVALID_SIZE; break; }   // Horizon asks for 2 MB multiples; a part page would leave the next change's mprotect unaligned
        pthread_mutex_lock(&g_map_lock);
        size_t previous = g_heap_cur;
        uint64_t image = module_image_bytes();
        if (size < previous && map_source_busy((uint64_t)g_heap + size, previous - size)) { pthread_mutex_unlock(&g_map_lock); regs[0] = R_INVALID_STATE; break; }
        if (image > g_p.app_pool || size > g_p.app_pool - image) { pthread_mutex_unlock(&g_map_lock); regs[0] = R_LIMIT_REACHED; break; }   // games retry in 2 MB decrements
        uint64_t charged = g_p.mem_used + (uint64_t)atomic_load(&g_alias_mapped);
        if (g_p.map_limit && (charged > g_p.app_pool || size > g_p.app_pool - charged)) { pthread_mutex_unlock(&g_map_lock); regs[0] = R_LIMIT_REACHED; break; }
        int rc = 0;
        if (size > previous) rc = mprotect(g_heap + previous, size - previous, PROT_READ | PROT_WRITE);
        else if (size < previous) {      // new heap memory is zero on Horizon: drop the pages as well as the access
            rc = mprotect(g_heap + size, previous - size, PROT_NONE);
            if (!rc) madvise(g_heap + size, previous - size, MADV_DONTNEED);
        }
        if (rc) die("SetHeapSize mprotect failed");
        g_heap_cur = size;
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; regs[1] = (uint64_t)g_heap; break;
    }
    case 0x02: case 0x03: {   // SetMemoryPermission / SetMemoryAttribute: accepted, but never on a borrowed source
        uint64_t addr = regs[0], size = regs[1];
        pthread_mutex_lock(&g_map_lock);
        int busy = size > UINT64_MAX - addr || (size && map_source_busy(addr, size));
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = busy ? R_INVALID_STATE : 0; break;
    }
    case 0x04: case 0x05: {   // MapMemory / UnmapMemory(dst, src, size): the SDK moves a thread's stack from the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t res = 0;
        if ((dst | src) & 0xfff) res = R_INVALID_ADDRESS;
        else if (!size || (size & 0xfff)) res = R_INVALID_SIZE;
        else if (size > UINT64_MAX - src || size > UINT64_MAX - dst) res = R_INVALID_STATE;
        else if (!contains((uint64_t)g_stack, g_stack_cap, dst, size)) res = R_INVALID_MEMREGION;
        if (res) { regs[0] = res; break; }
        pthread_mutex_lock(&g_map_lock);
        if (num == 0x04) {
            if (map_find(dst, size) || map_source_busy(src, size) || !map_source_backed(src, size)) res = R_INVALID_STATE;
            else if (g_nmap == MAX_MAP) res = R_OUT_OF_RESOURCE;
            if (!res) {
                if (mprotect((void *)dst, size, PROT_READ | PROT_WRITE)) die("MapMemory mprotect failed");
                memcpy((void *)dst, (void *)src, size);   // move semantics: the source is not accessed while mapped (Horizon makes it inaccessible)
                g_map[g_nmap++] = (typeof(g_map[0])){dst, size, src, 0x0B /*Stack*/};
            }
        } else {
            int k = map_find(dst, size);
            if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) res = R_INVALID_STATE;
            else {
                memcpy((void *)src, (void *)dst, size);   // the contents move back to the source
                mprotect((void *)dst, size, PROT_NONE);
                madvise((void *)dst, size, MADV_DONTNEED);
                g_map[k - 1] = g_map[--g_nmap];
            }
        }
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = res; break;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* x0, →x1 PageInfo; x2 address)
        uint64_t mi = regs[0], base, size, perm; uint32_t state, attr;
        pthread_mutex_lock(&g_map_lock);
        query(regs[2], &base, &size, &state, &perm, &attr);
        pthread_mutex_unlock(&g_map_lock);
        if (mi) {
            struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, reserved; } out = {base, size, state, attr, (uint32_t)perm, 0, 0, 0};
            _Static_assert(sizeof out == 40, "MemoryInfo ABI size");
            memcpy((void *)mi, &out, sizeof out);
        }
        regs[0] = 0; regs[1] = 0; break;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size): inside the alias region only (the SDK's VammManager)
        uint64_t a = regs[0], size = regs[1];
        if (!size || ((a | size) & 0xfff) || !contains((uint64_t)g_alias, g_alias_cap, a, size)) { regs[0] = R_INVALID_MEMREGION; break; }
        pthread_mutex_lock(&g_map_lock);
        // The SDK extends committed windows across borrowed stack sources; recommit preserves every live byte, only decommit can invalidate the backing
        if (num == 0x2D && map_source_busy(a, size)) { pthread_mutex_unlock(&g_map_lock); regs[0] = R_INVALID_STATE; break; }
        uint64_t off = a - (uint64_t)g_alias;
        int64_t need = alias_unmapped(off, size) * 4096;
        if (g_p.map_limit && num == 0x2C) {   // a map that would take the process past the application pool fails, and the SDK frees cached pages and retries
            int64_t room = (int64_t)g_p.app_pool - (int64_t)(g_heap_cur + g_p.mem_used) - atomic_load(&g_alias_mapped);
            if (need && need > room) { pthread_mutex_unlock(&g_map_lock); regs[0] = R_LIMIT_REACHED; break; }
        }
        int commit = num == 0x2C;
        if (commit) { if (mprotect((void *)a, size, PROT_READ | PROT_WRITE)) die("MapPhysicalMemory mprotect failed"); }
        else { mprotect((void *)a, size, PROT_NONE); madvise((void *)a, size, MADV_DONTNEED); }   // remapped pages must read as zero
        atomic_fetch_add(&g_alias_mapped, alias_pages(off, size, commit) * 4096);
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; break;
    }
    case 0x1E: regs[0] = kernel_cntpct(); break;   // GetSystemTick: the tick count itself in x0 (not a Result)
    case 0x26: {  // Break(reason, ptr, size): the SDK's fatal abort; bit 31 of the reason marks a notification that can continue
        (g_break ? g_break : default_break)(regs[0], regs[1], regs[2], cpu_get_x(c, 30));
        regs[0] = 0; break;
    }
    case 0x27: {  // OutputDebugString(str, len)
        if (getenv("NX_DBG")) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        regs[0] = 0; break;
    }
    case 0x29: {  // GetInfo(→x1 value; w1 id, w2 handle, x3 sub id)
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        // The SDK places thread stacks at random addresses inside the ASLR region and MapMemory only accepts the stack region. A 32-bit process's stack region is its whole ASLR region on a console;
        // here the stack region is a piece of the window, so an AArch32 title is told the ASLR region is exactly that piece (otherwise most of its random picks are refused and thread creation fails)
        case I_AslrAddr:  v = cpu_is_aarch32() ? (uint64_t)g_stack : GUEST_BASE;        break;   case I_AslrSize:  v = cpu_is_aarch32() ? g_stack_cap : guest_mem_size(); break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = g_p.app_pool; break;
        case I_UsedMem: case I_UsedNonSys: pthread_mutex_lock(&g_map_lock); v = used_memory(); pthread_mutex_unlock(&g_map_lock); break;
        // The system resource the NPDM declares. The SDK's IsVirtualAddressMemoryEnabled is "total != 0".
        case I_SysResTotal: v = g_p.sys_resource; break;
        case I_SysResUsed: v = g_p.sys_resource ? 0x400000 : 0; break;
        case I_RandomEntropy: if (getrandom(&v, sizeof v, 0) != (ssize_t)sizeof v) v = 0; break;
        case I_ProgramId: v = g_p.program_id; break;
        case I_IsApplication: v = 1; break;
        // nn::os::GetFreeThreadCount: the thread resource limit less the threads alive. pm gives an application 96 threads.
        case I_FreeThreadCount: { int u = kernel_live_threads(); v = u < 96 ? 96 - u : 0; break; }
        case I_UserExcAddr: { static uint64_t ctx; if (!ctx) ctx = guest_alloc(0x1000, 0x1000); v = ctx; break; }   // newer SDKs' SetDyingMessageRegion writes to +0x1c0
        default: v = 0; break;
        }
        regs[0] = 0; regs[1] = v; break;
    }
    default: handled = 0;
    }
    if (handled) for (int i = 0; i < 4; i++) cpu_set_x(c, i, regs[i]);
    return handled;
}
