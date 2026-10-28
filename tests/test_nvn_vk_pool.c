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
    CHECK(nvn_gpu_pools() == 3);

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

    // A pool removed, and added again over the same memory
    mtl_pool_del(c);
    CHECK(!mtl_addr((uint64_t)c, 4, &buf, &off) && nvn_gpu_pools() == 2);
    mtl_pool_add(c, POOL);
    CHECK(mtl_addr((uint64_t)c + 0x40, 4, &buf, &off) && off == 0x40 && nvn_gpu_pools() == 3);

    mtl_pool_del(a); mtl_pool_del(b); mtl_pool_del(c);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
