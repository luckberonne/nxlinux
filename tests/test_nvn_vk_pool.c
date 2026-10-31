// The Vulkan backend of the NVN layer on its own: memory pools imported from guest memory, address lookup, copies and fills inside a
// submission, dependencies between them, and fences that complete only when the GPU gets there.
#include "../src/host/guest_mem.h"
#include "../src/gfx/nvn/nvn_obj.h"
#include "../src/gfx/nvn/nvn_mtl.h"
#include "../src/gfx/nvn/nvn_vk.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)
#define POOL (1u << 20)

static uint8_t *alloc_pool(void) { return (uint8_t *)guest_alloc(POOL + 0x10000, 0x10000); }   // a gap after each pool, so "just past the end" is not the next pool

int main(void) {
    if (guest_mem_init(256ull << 20)) return 1;
    if (!nvn_gpu_ok()) { puts("SKIP: no Vulkan device"); return 0; }
    printf("device: %s\n", nvn_gpu_name());
    uint8_t *a = alloc_pool(), *b = alloc_pool(), *c = alloc_pool();
    mtl_pool_add(a, POOL); mtl_pool_add(b, POOL); mtl_pool_add(c, POOL);
    CHECK(nvn_gpu_pools() == 0);                                       // a pool's memory is imported when the GPU first needs it, not when the pool is made
    { void *b0; uint64_t o0; CHECK(mtl_addr((uint64_t)a + 8, 4, NULL, &o0) && nvn_gpu_pools() == 0); (void)b0; }   // asking only whether an address is in a pool imports nothing

    // Address lookup: GPU address == CPU address, resolved to (pool buffer, offset)
    void *buf; uint64_t off;
    CHECK(mtl_addr((uint64_t)a + 0x1234, 16, &buf, &off) && buf && off == 0x1234);
    CHECK(mtl_addr((uint64_t)b, POOL, &buf, &off) && off == 0);                       // the whole pool
    CHECK(!mtl_addr((uint64_t)a + POOL - 8, 16, &buf, &off));                        // straddles the end
    CHECK(!mtl_addr((uint64_t)a + POOL, 4, &buf, &off));                             // just past the pool
    CHECK(!mtl_addr(0x1000, 4, &buf, &off));                                         // not guest memory

    // CPU writes after the import are seen by the GPU; a copy between two pools
    for (uint32_t i = 0; i < POOL / 4; i++) ((uint32_t *)a)[i] = 0xA0000000u + i;
    mtl_sub_begin();
    mtl_sub_copy((uint64_t)a + 0x1000, (uint64_t)b + 0x2000, 0x3000);
    uint64_t f1 = mtl_sub_commit();
    CHECK(f1 >= 1 && mtl_wait(f1, 5000000000ull));
    int ok = 1; for (uint32_t i = 0; i < 0x3000 / 4; i++) if (((uint32_t *)(b + 0x2000))[i] != 0xA0000000u + 0x400 + i) ok = 0;
    printf("copy between pools: %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);
    CHECK(((uint32_t *)b)[0x1FFF / 4] == 0 && ((uint32_t *)b)[0x5000 / 4] == 0);       // the neighbours are untouched

    // Dependent operations in one submission: fill, copy the result on, copy it again; each reads what the one before wrote
    mtl_sub_begin();
    mtl_sub_fill((uint64_t)b, 0x4000, 0x5A5A5A5Au);
    mtl_sub_copy((uint64_t)b, (uint64_t)c + 0x100, 0x4000);
    mtl_sub_copy((uint64_t)c + 0x100, (uint64_t)a + 0x8000, 0x4000);
    uint64_t f2 = mtl_sub_commit();
    CHECK(f2 > f1 && mtl_last_fence() == f2);
    mtl_finish();
    ok = 1; for (uint32_t i = 0; i < 0x4000 / 4; i++) if (((uint32_t *)(a + 0x8000))[i] != 0x5A5A5A5Au || ((uint32_t *)(c + 0x100))[i] != 0x5A5A5A5Au) ok = 0;
    printf("fill -> copy -> copy in one submission: %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);

    // Fences: complete means the GPU reached it; a value not yet submitted times out
    CHECK(mtl_wait(f1, 0) && mtl_wait(f2, 0));                                        // both done: a zero timeout still answers yes
    CHECK(!mtl_wait(f2 + 50, 1000000));                                               // never submitted: times out after 1 ms
    CHECK(nvn_gpu_completed() >= f2);
    mtl_sub_begin(); uint64_t f3 = mtl_sub_commit();                                  // an empty submission still goes through the GPU
    CHECK(f3 == f2 + 1 && mtl_wait(f3, 5000000000ull));

    // Many submissions in a row, then the last one tells it all
    uint64_t last = 0;
    for (int i = 0; i < 200; i++) { mtl_sub_begin(); mtl_sub_copy((uint64_t)a, (uint64_t)b + 0x10000, 0x1000); last = mtl_sub_commit(); }
    CHECK(last == f3 + 200 && mtl_wait(last, 5000000000ull));
    mtl_drain();

    // Effects of completion: events and counter reports are written after the GPU gets there, in submission order, and are visible
    // once the fence they belong to is waited on
    volatile uint32_t *ev = (volatile uint32_t *)(c + 0x20000);
    *ev = 0;
    mtl_sub_begin(); mtl_sub_event(ev, 0, 5);  uint64_t e1 = mtl_sub_commit();      // write 5
    mtl_sub_begin(); mtl_sub_event(ev, 1, 10); uint64_t e2 = mtl_sub_commit();      // add 10
    mtl_sub_begin(); mtl_sub_event(ev, 1, 100); mtl_sub_event(ev, 0, 1); mtl_sub_event(ev, 1, 2); uint64_t e3 = mtl_sub_commit();   // add 100, write 1, add 2, in this order
    CHECK(e1 < e2 && e2 < e3);
    CHECK(mtl_wait(e1, 5000000000ull) && *ev >= 5);                                  // the first has landed (later ones may be on their way)
    CHECK(mtl_wait(e3, 5000000000ull) && *ev == 3);                                  // 5, +10, +100, =1, +2: the order held
    for (int i = 0; i < 100; i++) { mtl_sub_begin(); mtl_sub_event(ev, 1, 1); last = mtl_sub_commit(); }
    CHECK(mtl_wait(last, 5000000000ull) && *ev == 103);                             // none lost, none twice

    volatile uint64_t *rep = (volatile uint64_t *)(c + 0x21000);
    rep[0] = rep[1] = rep[2] = rep[3] = 0;
    mtl_sub_begin(); mtl_sub_counter((uint64_t)(uintptr_t)rep, 0); uint64_t r1 = mtl_sub_commit();          // timestamp
    CHECK(mtl_wait(r1, 5000000000ull));
    uint64_t t1 = rep[1];
    CHECK(t1 != 0 && rep[0] == t1);                                                  // a timestamp report carries the time in both words
    mtl_sub_begin(); mtl_sub_counter((uint64_t)(uintptr_t)(rep + 2), 0); uint64_t r2 = mtl_sub_commit();
    CHECK(mtl_wait(r2, 5000000000ull) && rep[3] >= t1);                              // time does not go back

    // Command lists built by hand: UPDATE_UB with a size and an offset that rule out vkCmdUpdateBuffer (staging path), CALL into a sub-list that
    // returns, JUMP to a list that ends the walk. A record is {op, size} + payload, size in 8-byte steps (nvn_obj.h).
    {
        uint8_t *L1 = (uint8_t *)guest_alloc(0x1000, 16), *L2 = (uint8_t *)guest_alloc(0x1000, 16), *L3 = (uint8_t *)guest_alloc(0x1000, 16);
        uint8_t data[101]; for (int i = 0; i < 101; i++) data[i] = (uint8_t)(0x40 + i);
        uint8_t *w = L2;                                                         // sub-list: copy 0x100 bytes of a to b+0x7000, then end
        { op_hdr h = {OP_COPY_B2B, 8 + 24}; uint64_t pl[3] = {(uint64_t)a + 0x100, (uint64_t)b + 0x7000, 0x100}; memcpy(w, &h, 8); memcpy(w + 8, pl, 24); w += h.size; }
        { op_hdr h = {OP_END, 8}; memcpy(w, &h, 8); }
        w = L3;                                                                  // reached by JUMP: fill 0x40 bytes of c, then end
        { op_hdr h = {OP_CLEAR_BUF, 8 + sizeof(p_clear_buf)}; p_clear_buf cb = {(uint64_t)c + 0x9000, 0x40, 0x11223344u, 0}; memcpy(w, &h, 8); memcpy(w + 8, &cb, sizeof cb); w += h.size; }
        { op_hdr h = {OP_END, 8}; memcpy(w, &h, 8); }
        w = L1;
        { op_hdr h = {OP_UPDATE_UB, 8 + 16 + 104}; uint64_t pl[2] = {(uint64_t)b + 0x5003, 101}; memcpy(w, &h, 8); memcpy(w + 8, pl, 16); memcpy(w + 24, data, 101); w += h.size; }
        { op_hdr h = {OP_CALL, 16}; uint64_t t = (uint64_t)(uintptr_t)L2; memcpy(w, &h, 8); memcpy(w + 8, &t, 8); w += h.size; }
        { op_hdr h = {OP_BARRIER, 16}; uint32_t bits = 1; memcpy(w, &h, 8); memcpy(w + 8, &bits, 4); w += h.size; }
        { op_hdr h = {OP_VIEWPORTS, 8 + sizeof(p_viewports)}; p_viewports v = {0}; memcpy(w, &h, 8); memcpy(w + 8, &v, sizeof v); w += h.size; }   // not executed yet: skipped, not fatal
        { op_hdr h = {OP_JUMP, 16}; uint64_t t = (uint64_t)(uintptr_t)L3; memcpy(w, &h, 8); memcpy(w + 8, &t, 8); }
        memset(b + 0x5000, 0, 0x200); memset(b + 0x7000, 0, 0x100); memset(c + 0x9000, 0, 0x40);
        mtl_sub_begin(); mtl_exec(L1); uint64_t lf = mtl_sub_commit();
        CHECK(mtl_wait(lf, 5000000000ull));
        int u_ok = 1; for (int i = 0; i < 101; i++) if (b[0x5003 + i] != data[i]) u_ok = 0;
        CHECK(u_ok && b[0x5002] == 0 && b[0x5003 + 101] == 0);                   // 101 bytes at an odd offset, neighbours untouched
        int c_ok = 1; for (int i = 0; i < 0x100 / 4; i++) if (((uint32_t *)(b + 0x7000))[i] != 0xA0000000u + 0x40 + (uint32_t)i) c_ok = 0;
        CHECK(c_ok);                                                              // the CALLed sub-list ran and returned
        int f_ok = 1; for (int i = 0; i < 0x40 / 4; i++) if (((uint32_t *)(c + 0x9000))[i] != 0x11223344u) f_ok = 0;
        CHECK(f_ok && ((uint32_t *)(c + 0x9000))[0x40 / 4] == 0);               // the JUMPed list ran
        printf("hand-built lists (UPDATE_UB staging, CALL, JUMP): %s\n", u_ok && c_ok && f_ok ? "ok" : "MISMATCH");
    }
    mtl_drain();

    // A pool removed, and added again over the same memory
    mtl_pool_del(c);
    CHECK(!mtl_addr((uint64_t)c, 4, &buf, &off) && nvn_gpu_pools() == 2);
    mtl_pool_add(c, POOL);
    CHECK(mtl_addr((uint64_t)c + 0x40, 4, &buf, &off) && off == 0x40 && nvn_gpu_pools() == 3);

    mtl_pool_del(a); mtl_pool_del(b); mtl_pool_del(c);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
