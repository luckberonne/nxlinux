// Guest program for tests/test_mem.cpp: the memory SVCs through raw svc instructions. Results go to g_res[].
typedef unsigned long u64;
typedef unsigned int u32;

#define SVC(num, a0, a1, a2, a3, r0, r1, r2) do { \
    register u64 x0 __asm__("x0") = (u64)(a0), x1 __asm__("x1") = (u64)(a1), x2 __asm__("x2") = (u64)(a2), x3 __asm__("x3") = (u64)(a3); \
    __asm__ volatile("svc %[n]" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : [n] "i"(num) : "memory", "x4", "x5", "x6", "x7"); \
    r0 = x0; r1 = x1; r2 = x2; } while (0)

struct mi { u64 base, size; u32 state, attr, perm, ipc, device, reserved; };

u64 g_res[64];
u64 g_info[8];                 // heap, heap cap, alias, alias size, stack, stack size, total, used
static struct mi g_mi;

static u64 get_info(u64 id) { u64 r, v, c; SVC(0x29, 0, id, 0, 0, r, v, c); return v; }
static u64 query(u64 addr) { u64 r, p, c; SVC(0x06, &g_mi, 0, addr, 0, r, p, c); return r; }
static u64 set_heap(u64 size, u64 *base) { u64 r, b, c; SVC(0x01, 0, size, 0, 0, r, b, c); if (base) *base = b; return r; }
static u64 map_mem(u64 dst, u64 src, u64 size) { u64 r, a, b; SVC(0x04, dst, src, size, 0, r, a, b); return r; }
static u64 unmap_mem(u64 dst, u64 src, u64 size) { u64 r, a, b; SVC(0x05, dst, src, size, 0, r, a, b); return r; }
static u64 map_phys(u64 a, u64 size) { u64 r, x, y; SVC(0x2C, a, size, 0, 0, r, x, y); return r; }
static u64 unmap_phys(u64 a, u64 size) { u64 r, x, y; SVC(0x2D, a, size, 0, 0, r, x, y); return r; }
static void exit_thread(void) { u64 a, b, c; SVC(0x0A, 0, 0, 0, 0, a, b, c); for (;;) {} }

void guest_main(u64 arg) {
    int n = 0;
    g_info[0] = get_info(4); g_info[1] = get_info(5); g_info[2] = get_info(2); g_info[3] = get_info(3);
    g_info[4] = get_info(14); g_info[5] = get_info(15); g_info[6] = get_info(6); g_info[7] = get_info(7);
    u64 heap = g_info[0], alias = g_info[2], stack = g_info[4];

    // ---- heap
    u64 base = 0;
    g_res[n++] = set_heap(0x1000000, &base);                  // 0: grow to 16 MB
    g_res[n++] = base == heap;                                // 1
    *(volatile u64 *)(heap + 0xFFFFF8) = 0x1122334455667788ull;   // the last word of the heap
    g_res[n++] = query(heap + 0x1000);                        // 2
    g_res[n++] = g_mi.base == heap && g_mi.size == 0x1000000 && g_mi.state == 5 && g_mi.perm == 3;   // 3
    g_res[n++] = set_heap(g_info[1] + 0x1000, 0);             // 4: more than the heap region: OutOfMemory 0xD001
    g_res[n++] = set_heap(0x3E00000 + 0x1000, 0);             // 5: past the pool less the images: LimitReached 0x10801
    g_res[n++] = set_heap(0x800000, 0);                       // 6: shrink to 8 MB
    g_res[n++] = query(heap + 0xFFFFF8);                      // 7: now free
    g_res[n++] = g_mi.state == 0 && g_mi.base == heap + 0x800000;   // 8
    set_heap(0x1000000, 0);                                   // grow again
    g_res[n++] = *(volatile u64 *)(heap + 0xFFFFF8);          // 9: new heap memory is zero, not the old value

    // ---- MapMemory / UnmapMemory: a thread stack moves from the heap into the stack region
    u64 src = heap + 0x100000, dst = stack + 0x10000;
    for (int i = 0; i < 0x4000 / 8; i++) ((volatile u64 *)src)[i] = 0xA000 + i;
    g_res[n++] = map_mem(dst, src, 0x4000);                   // 10
    g_res[n++] = ((volatile u64 *)dst)[5] == 0xA005;          // 11: contents arrived
    ((volatile u64 *)dst)[7] = 0xBEEF;                        // the thread used its stack
    query(dst);
    g_res[n++] = g_mi.state == 0xB && g_mi.base == dst && g_mi.size == 0x4000;   // 12
    query(src + 0x10);
    g_res[n++] = g_mi.attr == 1 && g_mi.perm == 0;            // 13: the source is borrowed
    g_res[n++] = set_heap(0x100000, 0);                       // 14: cannot shrink below a borrowed source: 0xD401
    g_res[n++] = map_mem(dst, src, 0x4000);                   // 15: already mapped: 0xD401
    g_res[n++] = map_mem(dst + 0x100, src, 0x4000);           // 16: misaligned: 0xCC01
    g_res[n++] = map_mem(dst, src, 0x123);                    // 17: bad size: 0xCA01
    g_res[n++] = map_mem(heap + 0x200000, src, 0x1000);       // 18: destination outside the stack region: 0xDC01
    g_res[n++] = unmap_mem(dst, src, 0x2000);                 // 19: does not match the mapping: 0xD401
    g_res[n++] = unmap_mem(dst, src, 0x4000);                 // 20
    g_res[n++] = ((volatile u64 *)src)[7] == 0xBEEF;          // 21: the contents went back
    g_res[n++] = set_heap(0x100000, 0);                       // 22: shrinking is allowed again

    // ---- MapPhysicalMemory in the alias region
    u64 a = alias + 0x10000;
    g_res[n++] = map_phys(a, 0x8000);                         // 23
    *(volatile u64 *)(a + 0x7FF8) = 0x77;
    query(a);
    g_res[n++] = g_mi.state == 5 && g_mi.base == a && g_mi.size == 0x8000;   // 24
    query(alias);
    g_res[n++] = g_mi.state == 0 && g_mi.size == 0x10000;     // 25: free up to the mapped run
    g_res[n++] = map_phys(a + 0x4000, 0x8000);                // 26: overlapping map keeps the live bytes
    g_res[n++] = *(volatile u64 *)(a + 0x7FF8);               // 27
    g_res[n++] = unmap_phys(a, 0xC000);                       // 28
    query(a);
    g_res[n++] = g_mi.state == 0;                             // 29
    g_res[n++] = map_phys(a, 0x1000);                         // 30
    g_res[n++] = *(volatile u64 *)a;                          // 31: a fresh page reads zero
    g_res[n++] = map_phys(a + 1, 0x1000);                     // 32: misaligned: 0xDC01
    g_res[n++] = map_phys(heap, 0x1000);                      // 33: outside the alias region: 0xDC01

    // ---- modules, free space, the host-made stack
    query((u64)guest_main);
    g_res[n++] = g_mi.state == 3 && g_mi.perm == 5;           // 34: code is Code+RX
    query(heap + 0x1000000 + 0x2000);                         // above the committed heap
    g_res[n++] = g_mi.state == 0 && g_mi.perm == 0;           // 35
    query((u64)&g_mi);
    g_res[n++] = g_mi.state == 4 && g_mi.perm == 3;           // 36: data is CodeData+RW

    // ---- Break and the tick counter
    u64 t1, t2, r, x, y;
    SVC(0x1E, 0, 0, 0, 0, t1, x, y);
    for (volatile int i = 0; i < 2000; i++) {}
    SVC(0x1E, 0, 0, 0, 0, t2, x, y);
    g_res[n++] = t2 > t1;                                     // 37
    SVC(0x26, 0x80000000ull, 0, 0, 0, r, x, y);               // a notification: execution continues
    g_res[n++] = 1;                                           // 38: still here
    g_res[n++] = get_info(1000) == 0;                         // 39: an unknown id answers 0
    g_res[n++] = 0x5555;                                      // 40: end marker
    exit_thread();
}
