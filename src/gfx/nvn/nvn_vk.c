// Memory pools, submissions and fences on Vulkan.
//
// A memory pool is guest memory (GPU address == CPU address). It is imported as Vulkan memory with VK_EXT_external_memory_host, so the CPU
// and the GPU see the same bytes with no copy, and bound to one buffer that covers the pool. An NVN buffer address then resolves to
// (pool buffer, offset) by an interval lookup, as the Metal layer does with its zero-copy MTLBuffers.
//
// A submission is one primary command buffer, recorded between mtl_sub_begin and mtl_sub_commit and submitted with a timeline semaphore
// signal. The fence value of a submission is the timeline value it signals: it completes when the GPU has actually reached it, and every
// visible effect of "GPU completion" waits for that, as nxnative's ordering convention requires.
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_vk.h"
#include "nvn_vk_priv.h"
#include "nvn_draw.h"
#include "nvn_timestamp.h"
#include "../vk/vkctx.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_POOLS 16384    // Mario Kart 8 Deluxe makes well over a thousand while a course loads
#define MAX_INFLIGHT 64

uint64_t guest_ns(void);                                 // the guest clock (hostsvc.c)
void dm_guest_write_prepare(void *addr, size_t len);     // the host is about to write guest memory (dirty tracking)

// Host actions that follow a submission's completion
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; } post_op;   // kind 1 event, 2 counter report
typedef struct { uint64_t value; int n, nstg; post_op *ops; vkbuf_t *stg; } sub_rec;

typedef struct {
    uint64_t base, size;               // as the game gave it
    uint64_t abase, asize;             // widened to the import alignment: what Vulkan sees
    VkBuffer buf; VkDeviceMemory mem; VkDeviceAddress gpu;
    int tried;                         // the memory is imported when the GPU first needs the pool, not when the game makes it (pool_ensure): most pools are never used by the GPU
} gpool_t;

static struct {
    pthread_once_t once; vkctx_t c; int ok;
    VkSemaphore tl;
    pthread_mutex_t pool_mu; gpool_t pool[MAX_POOLS]; int npool;
    VkCommandBuffer cur;                                 // the submission being recorded
    struct { VkCommandBuffer cb; uint64_t value; } flight[MAX_INFLIGHT]; int nflight;
    _Atomic uint64_t submitted;                          // last timeline value handed to the queue
    // the submission being recorded: actions to run after it completes, and staging buffers it reads
    post_op *ops; int nops, capops; vkbuf_t *stg; int nstg, capstg;
    // submissions waiting for the GPU, oldest first; the completion thread and any waiter drain it
    pthread_mutex_t vk_mu;                              // the queue and the command pool are externally synchronized: submit, one-shot readbacks and command buffer recycling take it
    struct dead_ent { VkImage img; VkImageView view; VkDeviceMemory mem; struct vkmem_sub *sub; uint64_t value; } *dead; int ndead, cdead;   // images to destroy once the timeline passes `value`
    pthread_mutex_t q_mu; pthread_cond_t q_cv; sub_rec *q; size_t qhead, qn, qcap; pthread_t thread; int thread_up;
} G = {.once = PTHREAD_ONCE_INIT, .pool_mu = PTHREAD_MUTEX_INITIALIZER, .vk_mu = PTHREAD_MUTEX_INITIALIZER, .q_mu = PTHREAD_MUTEX_INITIALIZER, .q_cv = PTHREAD_COND_INITIALIZER};

static void gpu_init(void) {
    if (getenv("NX_NULL_GPU")) { fprintf(stderr, "[dm] [nvn] NX_NULL_GPU set: no Vulkan device is opened\n"); return; }
    if (vkctx_create(&G.c)) { fprintf(stderr, "[dm] [RED] [nvn] no usable Vulkan device: running on the null GPU\n"); return; }
    if (!G.c.has_ext_mem_host) { fprintf(stderr, "[dm] [RED] [nvn] %s lacks VK_EXT_external_memory_host: running on the null GPU\n", G.c.props.deviceName); return; }
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (vkCreateSemaphore(G.c.dev, &si, NULL, &G.tl)) { fprintf(stderr, "[dm] [RED] [nvn] timeline semaphore creation failed\n"); return; }
    G.ok = 1;
    fprintf(stderr, "[dm] [nvn] Vulkan backend on %s%s\n", G.c.props.deviceName, G.c.validation ? " (validation layers on)" : "");
}
static inline int gpu(void) { pthread_once(&G.once, gpu_init); return G.ok; }
int nvn_gpu_ok(void) { return gpu(); }
const char *nvn_gpu_name(void) { return gpu() ? G.c.props.deviceName : "none"; }
unsigned nvn_gpu_pools(void) { unsigned n = 0; pthread_mutex_lock(&G.pool_mu); for (int i = 0; i < G.npool; i++) n += G.pool[i].buf != VK_NULL_HANDLE; pthread_mutex_unlock(&G.pool_mu); return n; }
uint64_t nvn_gpu_completed(void) { uint64_t v = 0; if (gpu()) vkGetSemaphoreCounterValue(G.c.dev, G.tl, &v); return v; }

vkctx_t *nvn_vk_ctx(void) { return gpu() ? &G.c : NULL; }
VkCommandBuffer nvn_vk_cmd_raw(void) { return G.ok ? G.cur : NULL; }
// Every other operation (copies, fills, barriers, clears) is invalid inside a render pass instance: asking for the command buffer ends the draw path's pass
VkCommandBuffer nvn_vk_cmd(void) { if (!G.ok || !G.cur) return NULL; nvn_draw_pass_end(); if (G.cur) nvn_vk_barrier_flush(G.cur); return G.cur; }
uint64_t nvn_vk_next_fence(void) { return gpu() ? atomic_load(&G.submitted) + 1 : 0; }

// ---- effects of completion
static void post_add(post_op o) {
    if (G.nops == G.capops) { G.capops = G.capops ? G.capops * 2 : 64; G.ops = realloc(G.ops, (size_t)G.capops * sizeof *G.ops); }
    G.ops[G.nops++] = o;
}
void nvn_vk_post_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post_add((post_op){1, val, mode, v, 0, 0}); }
void nvn_vk_post_counter(uint64_t addr, uint32_t type) { post_add((post_op){2, NULL, 0, 0, addr, type}); }
// ---- transient uniform arena: chunks of TR_CHUNK bytes; a chunk is tagged with the last submission that took from it and handed out again once that one completed
enum { TR_CHUNK = 4 << 20, TR_MAX = 64 };
static struct { vkbuf_t b; size_t used; uint64_t fence; } g_tr[TR_MAX]; static int g_ntr, g_trcur = -1;
static pthread_mutex_t g_tr_mu = PTHREAD_MUTEX_INITIALIZER;
int nvn_vk_transient(size_t size, VkDescriptorBufferInfo *out, void **map) {
    if (!gpu() || size > TR_CHUNK) return -1;
    static size_t align; if (!align) { align = G.c.props.limits.minUniformBufferOffsetAlignment; if (align < 16) align = 16; }
    uint64_t fence = nvn_vk_next_fence();
    pthread_mutex_lock(&g_tr_mu);
    if (g_trcur >= 0 && ((g_tr[g_trcur].used + align - 1) & ~(align - 1)) + size > TR_CHUNK) g_trcur = -1;
    if (g_trcur < 0) {
        uint64_t done = nvn_gpu_completed();
        for (int i = 0; i < g_ntr && g_trcur < 0; i++) if (g_tr[i].fence <= done) { g_trcur = i; g_tr[i].used = 0; }
        if (g_trcur < 0 && g_ntr < TR_MAX && !vkbuf_create(&G.c, &g_tr[g_ntr].b, TR_CHUNK, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1)) { g_tr[g_ntr].used = 0; g_trcur = g_ntr++; }
        if (g_trcur < 0) { pthread_mutex_unlock(&g_tr_mu); return -1; }   // every chunk is still in flight: the caller makes its own buffer
    }
    size_t off = (g_tr[g_trcur].used + align - 1) & ~(align - 1);
    g_tr[g_trcur].used = off + size; g_tr[g_trcur].fence = fence;
    *out = (VkDescriptorBufferInfo){g_tr[g_trcur].b.buf, off, size};
    *map = (uint8_t *)g_tr[g_trcur].b.map + off;
    pthread_mutex_unlock(&g_tr_mu);
    return 0;
}

void nvn_vk_keep(const vkbuf_t *b) {
    if (G.nstg == G.capstg) { G.capstg = G.capstg ? G.capstg * 2 : 8; G.stg = realloc(G.stg, (size_t)G.capstg * sizeof *G.stg); }
    G.stg[G.nstg++] = *b;
}
static void post_run(post_op *ops, int n) {
    for (int i = 0; i < n; i++) {
        post_op *o = &ops[i];
        if (o->kind == 1) {
            dm_guest_write_prepare((void *)o->val, 4);
            if (o->mode == 1) __atomic_fetch_add(o->val, o->v, __ATOMIC_RELEASE); else __atomic_store_n(o->val, o->v, __ATOMIC_RELEASE);
        } else {
            // A counter report is {counter, timestamp}; the timestamp counts 614.4 MHz ticks of the guest clock. Timestamp types report the
            // time in both words, other counters (not measured here yet) report 0 as their count.
            uint64_t t = nvn_timestamp_from_ns(guest_ns()), rec[2] = {(o->type == 0 || o->type == 0x10) ? t : 0, t};
            dm_guest_write_prepare((void *)(uintptr_t)o->addr, sizeof rec);
            memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
        }
    }
}
// Run the records of every submission the GPU has completed, oldest first. Holding q_mu while the writes land makes a second caller wait
// for them: whoever sees a fence complete has also seen its effects, which is the ordering NVN's events and sync objects rely on.
static void post_drain(uint64_t completed) {
    pthread_mutex_lock(&G.q_mu);
    while (G.qn && G.q[G.qhead].value <= completed) {
        sub_rec r = G.q[G.qhead]; G.qhead = (G.qhead + 1) % G.qcap; G.qn--;
        post_run(r.ops, r.n);
        for (int i = 0; i < r.nstg; i++) vkbuf_destroy(&G.c, &r.stg[i]);
        free(r.ops); free(r.stg);
    }
    pthread_mutex_unlock(&G.q_mu);
}
static void *completion_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&G.q_mu);
        while (!G.qn) pthread_cond_wait(&G.q_cv, &G.q_mu);
        uint64_t v = G.q[G.qhead].value;
        pthread_mutex_unlock(&G.q_mu);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &G.tl, .pValues = &v};
        if (vkWaitSemaphores(G.c.dev, &wi, 100000000ull) == VK_SUCCESS) post_drain(v);
    }
    return NULL;
}
static void post_enqueue(uint64_t value) {      // the submission just committed: hand its actions to the completion side
    if (!G.nops && !G.nstg) return;
    sub_rec r = {value, G.nops, G.nstg, G.ops, G.stg};
    G.ops = NULL; G.stg = NULL; G.nops = G.capops = G.nstg = G.capstg = 0;
    pthread_mutex_lock(&G.q_mu);
    if (G.qn == G.qcap) {                        // grow the ring, keeping the order
        size_t nc = G.qcap ? G.qcap * 2 : 64; sub_rec *nq = malloc(nc * sizeof *nq);
        for (size_t i = 0; i < G.qn; i++) nq[i] = G.q[(G.qhead + i) % G.qcap];
        free(G.q); G.q = nq; G.qcap = nc; G.qhead = 0;
    }
    G.q[(G.qhead + G.qn++) % G.qcap] = r;
    if (!G.thread_up) { G.thread_up = 1; pthread_create(&G.thread, NULL, completion_thread, NULL); pthread_detach(G.thread); }
    pthread_cond_signal(&G.q_cv);
    pthread_mutex_unlock(&G.q_mu);
}

// ---- memory pools
static void pool_import(gpool_t *p) {
    if (getenv("NX_POOL_LOG")) fprintf(stderr, "[dm] [nvn] pool import %#llx %#llx\n", (unsigned long long)p->base, (unsigned long long)p->size);     // NX_POOL_LOG=1: every pool the title makes (base, size)
    uint64_t al = G.c.host_import_align ? G.c.host_import_align : 4096;
    p->abase = p->base & ~(al - 1); p->asize = ((p->base + p->size + al - 1) & ~(al - 1)) - p->abase;
    void *host = (void *)(uintptr_t)p->abase;
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = G.c.GetMemoryHostPointerProperties(G.c.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    if (r) { fprintf(stderr, "[dm] [RED] [nvn] pool %#llx+%#llx: the host pointer cannot be imported (%d)\n", (unsigned long long)p->base, (unsigned long long)p->size, (int)r); return; }
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, .size = p->asize, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (vkCreateBuffer(G.c.dev, &bi, NULL, &p->buf)) { fprintf(stderr, "[dm] [RED] [nvn] pool buffer of %#llx bytes could not be created\n", (unsigned long long)p->asize); p->buf = VK_NULL_HANDLE; return; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(G.c.dev, p->buf, &mr);
    uint32_t mt = vkctx_memtype(&G.c, mr.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, &fi, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, p->asize, mt};
    if (mt == UINT32_MAX || nx_vk_alloc(G.c.dev, &ai, &p->mem) || vkBindBufferMemory(G.c.dev, p->buf, p->mem, 0)) {
        fprintf(stderr, "[dm] [RED] [nvn] pool %#llx+%#llx: importing the memory failed\n", (unsigned long long)p->base, (unsigned long long)p->size);
        vkDestroyBuffer(G.c.dev, p->buf, NULL); p->buf = VK_NULL_HANDLE; p->mem = VK_NULL_HANDLE; return;
    }
    VkBufferDeviceAddressInfo di = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = p->buf};
    p->gpu = vkGetBufferDeviceAddress(G.c.dev, &di);
}
static void pool_release(gpool_t *p) {
    if (!p->buf) return;
    vkDeviceWaitIdle(G.c.dev);                            // the GPU may still read it: the guest is about to reuse or free this memory
    vkDestroyBuffer(G.c.dev, p->buf, NULL); nx_vk_free(G.c.dev, p->mem);
    p->buf = VK_NULL_HANDLE; p->mem = VK_NULL_HANDLE;
}

static int pool_slot(uint64_t base) {                     // the first entry whose base is >= base: caller holds pool_mu
    int lo = 0, hi = G.npool;
    while (lo < hi) { int m = (lo + hi) / 2; if (G.pool[m].base < base) lo = m + 1; else hi = m; }
    return lo;
}
void mtl_pool_add(const void *mem, uint64_t size) {
    int on = gpu();
    pthread_mutex_lock(&G.pool_mu);
    uint64_t base = (uint64_t)(uintptr_t)mem;
    int i = pool_slot(base);                              // the table is sorted by base, for pool_find's binary search
    int same = i < G.npool && G.pool[i].base == base;
    if (!same && G.npool == MAX_POOLS) { pthread_mutex_unlock(&G.pool_mu); fprintf(stderr, "[dm] [RED] [nvn] pool table full\n"); return; }
    if (same && on) pool_release(&G.pool[i]);             // the same base again: a pool re-created over its memory
    if (!same) { memmove(&G.pool[i + 1], &G.pool[i], (size_t)(G.npool - i) * sizeof G.pool[0]); G.npool++; }
    gpool_t *p = &G.pool[i];
    memset(p, 0, sizeof *p); p->base = base; p->size = size;
    if (on && getenv("NX_POOL_EAGER")) { p->tried = 1; pool_import(p); }       // NX_POOL_EAGER=1: import at creation, as before
    pthread_mutex_unlock(&G.pool_mu);
}
void mtl_pool_del(const void *mem) {
    int on = gpu();
    pthread_mutex_lock(&G.pool_mu);
    int i = pool_slot((uint64_t)(uintptr_t)mem);
    if (i < G.npool && G.pool[i].base == (uint64_t)(uintptr_t)mem) {
        if (on) pool_release(&G.pool[i]);
        memmove(&G.pool[i], &G.pool[i + 1], (size_t)(G.npool - i - 1) * sizeof G.pool[0]); G.npool--;
    }
    pthread_mutex_unlock(&G.pool_mu);
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }

// The pool that holds [addr, addr+len): caller holds pool_mu. The returned pointer is valid until the table changes.
static gpool_t *pool_find_raw(uint64_t addr, uint64_t len) {
    int i = pool_slot(addr + 1) - 1;                      // the last pool starting at or below addr (pools do not overlap)
    if (i < 0) return NULL;
    gpool_t *p = &G.pool[i];
    if (addr >= p->base && len <= p->size && addr - p->base <= p->size - len) return p;
    return NULL;
}
static void pool_ensure(gpool_t *p) { if (p && !p->tried) { p->tried = 1; if (gpu()) pool_import(p); } }     // caller holds pool_mu
static gpool_t *pool_find(uint64_t addr, uint64_t len) { gpool_t *p = pool_find_raw(addr, len); pool_ensure(p); return p; }
int nvn_vk_pool_buffer(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    pthread_mutex_lock(&G.pool_mu);
    gpool_t *p = pool_find(addr, len);
    int ok = p && p->buf;
    if (ok) { *buf = p->buf; *off = addr - p->abase; }
    pthread_mutex_unlock(&G.pool_mu);
    return ok;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    pthread_mutex_lock(&G.pool_mu);
    gpool_t *p = pool_find_raw(addr, len);
    if (p && buf) pool_ensure(p);                         // asked only whether the range is in a pool: nothing to import
    if (p) { if (buf) *buf = (void *)(uintptr_t)p->buf; if (off) *off = addr - p->abase; }
    pthread_mutex_unlock(&G.pool_mu);
    return p != NULL;
}

// ---- submissions
// ---- device memory for images, given out in pieces of blocks: every vkAllocateMemory is a buffer object the driver checks at each submission (the cost of a submission grew with the number of
// textures: 0.03 ms with 800, over 3 ms with 5000), so images share 64 MB blocks, with a free list per block; an image of 16 MB or more keeps a block of its own
typedef struct vkmem_blk { VkDeviceMemory mem; uint32_t mt; VkDeviceSize size, used; struct { VkDeviceSize off, size; } *fr; int nfr, cfr; int dedicated; struct vkmem_blk *next; } vkmem_blk_t;
struct vkmem_sub { VkDeviceMemory mem; VkDeviceSize off, size; vkmem_blk_t *blk; };
static vkmem_blk_t *g_mblk; static pthread_mutex_t g_mem_mu = PTHREAD_MUTEX_INITIALIZER;
enum { MEM_BLOCK = 64 << 20, MEM_BIG = 16 << 20 };
static vkmem_blk_t *mem_block_new(uint32_t mt, VkDeviceSize size, int dedicated) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size, mt};
    vkmem_blk_t *b = calloc(1, sizeof *b); if (!b) return NULL;
    if (nx_vk_alloc(G.c.dev, &ai, &b->mem)) { free(b); return NULL; }
    b->mt = mt; b->size = size; b->dedicated = dedicated;
    if (!dedicated) { b->cfr = 64; b->fr = malloc((size_t)b->cfr * sizeof *b->fr); b->fr[0].off = 0; b->fr[0].size = size; b->nfr = 1; }
    b->next = g_mblk; g_mblk = b;
    return b;
}
static int mem_carve(vkmem_blk_t *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off_out) {     // first fit in the block; caller holds g_mem_mu
    for (int i = 0; i < b->nfr; i++) {
        VkDeviceSize o = b->fr[i].off, e = o + b->fr[i].size, a = (o + align - 1) & ~(align - 1);
        if (a + size > e) continue;
        int pre = a > o, post = a + size < e;
        if (pre && post) { if (b->nfr == b->cfr) { b->cfr *= 2; b->fr = realloc(b->fr, (size_t)b->cfr * sizeof *b->fr); } memmove(&b->fr[i + 1], &b->fr[i], (size_t)(b->nfr - i) * sizeof *b->fr); b->nfr++; b->fr[i].size = a - o; b->fr[i + 1].off = a + size; b->fr[i + 1].size = e - a - size; }
        else if (pre) b->fr[i].size = a - o;
        else if (post) { b->fr[i].off = a + size; b->fr[i].size = e - a - size; }
        else { memmove(&b->fr[i], &b->fr[i + 1], (size_t)(b->nfr - i - 1) * sizeof *b->fr); b->nfr--; }
        b->used += size; *off_out = a; return 1;
    }
    return 0;
}
struct vkmem_sub *nvn_vk_mem_alloc(const VkMemoryRequirements *mr, uint32_t mt) {
    if (!gpu()) return NULL;
    struct vkmem_sub *s = malloc(sizeof *s); if (!s) return NULL;
    VkDeviceSize size = (mr->size + 255) & ~(VkDeviceSize)255, align = mr->alignment ? mr->alignment : 256;
    pthread_mutex_lock(&g_mem_mu);
    vkmem_blk_t *b = NULL; VkDeviceSize off = 0;
    static int dedicated_all = -1; if (dedicated_all < 0) dedicated_all = getenv("NX_MEM_DEDICATED") != NULL;       // NX_MEM_DEDICATED=1: every image keeps a block of its own, as before the shared blocks
    if (size >= MEM_BIG || dedicated_all) { b = mem_block_new(mt, size, 1); if (b) { b->used = size; off = 0; } }
    else {
        for (b = g_mblk; b; b = b->next) if (!b->dedicated && b->mt == mt && mem_carve(b, size, align, &off)) break;
        if (!b && (b = mem_block_new(mt, MEM_BLOCK, 0)) && !mem_carve(b, size, align, &off)) b = NULL;
        if (!b) { b = mem_block_new(mt, size, 1); if (b) { b->used = size; off = 0; } }       // no room for a block: the image alone
    }
    pthread_mutex_unlock(&g_mem_mu);
    if (!b) { free(s); return NULL; }
    s->mem = b->mem; s->off = off; s->size = size; s->blk = b;
    return s;
}
void nvn_vk_mem_free(struct vkmem_sub *s) {
    if (!s) return;
    vkmem_blk_t *b = s->blk;
    pthread_mutex_lock(&g_mem_mu);
    b->used -= s->size;
    int release = b->dedicated;
    if (!b->dedicated) {                                                           // the range goes back in order and joins its neighbours
        int i = 0; while (i < b->nfr && b->fr[i].off < s->off) i++;
        if (b->nfr == b->cfr) { b->cfr *= 2; b->fr = realloc(b->fr, (size_t)b->cfr * sizeof *b->fr); }
        memmove(&b->fr[i + 1], &b->fr[i], (size_t)(b->nfr - i) * sizeof *b->fr); b->nfr++; b->fr[i].off = s->off; b->fr[i].size = s->size;
        if (i + 1 < b->nfr && b->fr[i].off + b->fr[i].size == b->fr[i + 1].off) { b->fr[i].size += b->fr[i + 1].size; memmove(&b->fr[i + 1], &b->fr[i + 2], (size_t)(b->nfr - i - 2) * sizeof *b->fr); b->nfr--; }
        if (i > 0 && b->fr[i - 1].off + b->fr[i - 1].size == b->fr[i].off) { b->fr[i - 1].size += b->fr[i].size; memmove(&b->fr[i], &b->fr[i + 1], (size_t)(b->nfr - i - 1) * sizeof *b->fr); b->nfr--; }
        if (b->used == 0) {                                                        // an empty block goes back to the driver, unless it is the only empty one of its type (kept for the next images)
            int other_empty = 0; for (vkmem_blk_t *o = g_mblk; o; o = o->next) if (o != b && !o->dedicated && o->mt == b->mt && o->used == 0) other_empty = 1;
            release = other_empty;
        }
    }
    if (release) {
        for (vkmem_blk_t **p = &g_mblk; *p; p = &(*p)->next) if (*p == b) { *p = b->next; break; }
        nx_vk_free(G.c.dev, b->mem); free(b->fr); free(b);
    }
    pthread_mutex_unlock(&g_mem_mu);
    free(s);
}
VkDeviceMemory nvn_vk_mem_handle(const struct vkmem_sub *s) { return s->mem; }
VkDeviceSize nvn_vk_mem_offset(const struct vkmem_sub *s) { return s->off; }
static void reap(void) {                                  // return command buffers and images the GPU has finished with
    uint64_t done = nvn_gpu_completed();
    pthread_mutex_lock(&G.vk_mu);
    int k = 0;
    for (int i = 0; i < G.nflight; i++) {
        if (G.flight[i].value <= done) vkFreeCommandBuffers(G.c.dev, G.c.pool, 1, &G.flight[i].cb);
        else G.flight[k++] = G.flight[i];
    }
    G.nflight = k;
    k = 0;
    for (int i = 0; i < G.ndead; i++) {
        if (G.dead[i].value <= done) { vkDestroyImageView(G.c.dev, G.dead[i].view, NULL); vkDestroyImage(G.c.dev, G.dead[i].img, NULL); if (G.dead[i].sub) nvn_vk_mem_free(G.dead[i].sub); else nx_vk_free(G.c.dev, G.dead[i].mem); }
        else G.dead[k++] = G.dead[i];
    }
    G.ndead = k;
    pthread_mutex_unlock(&G.vk_mu);
}
// An image the game released: destroyed once every submission that may still use it has completed
static void defer_push(VkImage img, VkImageView view, VkDeviceMemory mem, struct vkmem_sub *sub) {
    if (!gpu()) return;
    pthread_mutex_lock(&G.vk_mu);
    uint64_t v = atomic_load(&G.submitted) + (G.cur ? 1 : 0);          // the submission being recorded counts: it may already mention the image
    if (G.ndead == G.cdead) { G.cdead = G.cdead ? G.cdead * 2 : 512; G.dead = realloc(G.dead, (size_t)G.cdead * sizeof *G.dead); }   // a batch of evictions makes hundreds at once: no waiting for the GPU to be idle
    G.dead[G.ndead++] = (struct dead_ent){img, view, mem, sub, v};
    pthread_mutex_unlock(&G.vk_mu);
}
void nvn_vk_defer_image(VkImage img, VkImageView view, VkDeviceMemory mem) { defer_push(img, view, mem, NULL); }
void nvn_vk_defer_image_sub(VkImage img, VkImageView view, struct vkmem_sub *sub) { defer_push(img, view, VK_NULL_HANDLE, sub); }
// A command buffer of its own, submitted and waited for at once: for readbacks that cannot wait for a fence the game has not asked for
VkCommandBuffer nvn_vk_oneshot_begin(void) {
    if (!gpu()) return NULL;
    pthread_mutex_lock(&G.vk_mu);
    VkCommandBuffer cb = vkctx_begin(&G.c);
    pthread_mutex_unlock(&G.vk_mu);
    return cb;
}
// The same command buffer submitted without waiting: a fence says when it is done, nvn_vk_oneshot_wait blocks for it (no lock held) and nvn_vk_oneshot_release frees what it used
_Atomic uint64_t g_oneshot_ns, g_oneshot_n;            // the time the one-shot submissions took (the present's window copy), for the per-frame statistics
int nvn_vk_oneshot_submit(VkCommandBuffer cb, VkFence *fence) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    pthread_mutex_lock(&G.vk_mu);
    VkResult r = vkCreateFence(G.c.dev, &fi, NULL, fence);
    if (!r) r = vkQueueSubmit(G.c.queue, 1, &si, *fence);
    pthread_mutex_unlock(&G.vk_mu);
    { struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1); atomic_fetch_add(&g_oneshot_ns, (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec))); atomic_fetch_add(&g_oneshot_n, 1); }
    return r ? -1 : 0;
}
int nvn_vk_oneshot_wait(VkFence fence) { return vkWaitForFences(G.c.dev, 1, &fence, VK_TRUE, 10ull * 1000000000ull) == VK_SUCCESS ? 0 : -1; }
void nvn_vk_oneshot_release(VkCommandBuffer cb, VkFence fence) {
    pthread_mutex_lock(&G.vk_mu);
    if (fence) vkDestroyFence(G.c.dev, fence, NULL);
    if (cb) vkFreeCommandBuffers(G.c.dev, G.c.pool, 1, &cb);
    pthread_mutex_unlock(&G.vk_mu);
}
int nvn_vk_oneshot_end(VkCommandBuffer cb) {
    pthread_mutex_lock(&G.vk_mu);
    int r = vkctx_end(&G.c, cb);                          // ends, submits, waits on a fence, frees
    pthread_mutex_unlock(&G.vk_mu);
    return r;
}
void mtl_sub_begin(void) {
    if (!gpu()) return;
    reap();
    pthread_mutex_lock(&G.vk_mu);
    G.cur = vkctx_begin(&G.c);
    pthread_mutex_unlock(&G.vk_mu);
    nvn_draw_new_submission();
    if (!G.cur) fprintf(stderr, "[dm] [RED] [nvn] could not start a command buffer\n");
}
uint64_t mtl_sub_commit(void) {
    if (!gpu()) {          // the null GPU completes the moment it is committed
        static _Atomic uint64_t n;
        post_run(G.ops, G.nops); for (int i = 0; i < G.nstg; i++) vkbuf_destroy(&G.c, &G.stg[i]);
        G.nops = G.nstg = 0;
        return atomic_fetch_add(&n, 1) + 1;
    }
    nvn_draw_pass_end();                   // a render pass cannot cross the end of the command buffer
    if (G.cur) nvn_vk_barrier_flush(G.cur);   // emit anything the deferred marks left pending
    uint64_t value = atomic_load(&G.submitted) + 1;
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = G.cur};
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = G.tl, .value = value, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = G.cur ? 1 : 0, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    if (G.cur) vkEndCommandBuffer(G.cur);
    pthread_mutex_lock(&G.vk_mu);
    VkResult r = vkQueueSubmit2(G.c.queue, 1, &si, VK_NULL_HANDLE);
    pthread_mutex_unlock(&G.vk_mu);
    if (r) fprintf(stderr, "[dm] [RED] [nvn] vkQueueSubmit2 failed (%d)\n", (int)r);
    if (G.cur) { if (G.nflight < MAX_INFLIGHT) { G.flight[G.nflight].cb = G.cur; G.flight[G.nflight++].value = value; } else { vkQueueWaitIdle(G.c.queue); vkFreeCommandBuffers(G.c.dev, G.c.pool, 1, &G.cur); } }
    G.cur = NULL;
    atomic_store(&G.submitted, value);
    post_enqueue(value);
    return value;
}
// Hands what has been recorded so far to the GPU and goes on in a new command buffer: the GPU starts the frame's first passes while the host is still translating the rest, instead of
// waiting for the whole frame. The fences of the segments are in queue order, so the frame's own (the last) still completes last. Called between two passes.
void mtl_sub_cut(void) {
    if (!gpu() || !G.cur) return;
    mtl_sub_commit();
    mtl_sub_begin();
}
uint64_t mtl_last_fence(void) { return gpu() ? atomic_load(&G.submitted) : 0; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (!gpu() || !fence) return 1;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &G.tl, .pValues = &fence};
    if (vkWaitSemaphores(G.c.dev, &wi, timeout_ns) != VK_SUCCESS) return 0;
    post_drain(fence);                                    // the effects of everything up to the fence land before this returns
    return 1;
}
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_finish(void) { if (gpu()) mtl_wait(atomic_load(&G.submitted), UINT64_MAX); }
void mtl_drain(void) { mtl_finish(); }

// ---- operations inside the submission being recorded
_Atomic uint64_t g_nvn_nbarrier;                          // counted for the per-frame statistics (nvn_replay_report)
static inline uint64_t barrier_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
_Atomic uint64_t g_nvn_barrier_ns;                        // host time spent recording full barriers (nvn_replay_report)
static _Atomic int g_bar_pending;                          // a full barrier is needed before the next read of what was written (deferred: many marks collapse into one)
_Atomic uint64_t g_nvn_nbarrier_emitted;                  // full barriers actually emitted (reported as the barrier count)
void nvn_vk_barrier_all(VkCommandBuffer cb) {              // immediate full barrier (used by OP_BARRIER and OP_UPDATE_UB)
    if (!cb) return;
    atomic_fetch_add(&g_nvn_nbarrier, 1);
    uint64_t t0 = barrier_now();
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    atomic_fetch_add(&g_nvn_barrier_ns, barrier_now() - t0);
    atomic_fetch_add(&g_nvn_nbarrier_emitted, 1);
}
void nvn_vk_barrier_mark(void) { atomic_store(&g_bar_pending, 1); }   // deferred: pass_end only marks; the next flush emits
void nvn_vk_barrier_flush(VkCommandBuffer cb) {
    if (!cb || !atomic_exchange(&g_bar_pending, 0)) return;
    uint64_t t0 = barrier_now();
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    atomic_fetch_add(&g_nvn_barrier_ns, barrier_now() - t0);
    atomic_fetch_add(&g_nvn_nbarrier_emitted, 1);
}
static void barrier_transfer(VkCommandBuffer cb) {        // later copies and fills may depend on this one
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!G.ok || !G.cur || !n) return;
    pthread_mutex_lock(&G.pool_mu);
    gpool_t *a = pool_find(src, n), *b = pool_find(dst, n);
    VkBuffer sb = a ? a->buf : VK_NULL_HANDLE, db = b ? b->buf : VK_NULL_HANDLE;
    VkBufferCopy rg = {a ? src - a->abase : 0, b ? dst - b->abase : 0, n};
    pthread_mutex_unlock(&G.pool_mu);
    if (!sb || !db) { fprintf(stderr, "[dm] [RED] [nvn] buffer copy %#llx -> %#llx (%#llx bytes): an address is outside the memory pools\n", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); return; }
    vkCmdCopyBuffer(G.cur, sb, db, 1, &rg);
    barrier_transfer(G.cur);
}
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (!G.ok || !G.cur || !n) return;
    pthread_mutex_lock(&G.pool_mu);
    gpool_t *b = pool_find(dst, n);
    VkBuffer db = b ? b->buf : VK_NULL_HANDLE; uint64_t off = b ? dst - b->abase : 0;
    pthread_mutex_unlock(&G.pool_mu);
    if (!db || (off & 3)) { fprintf(stderr, "[dm] [RED] [nvn] buffer fill %#llx (%#llx bytes) is outside the memory pools or unaligned\n", (unsigned long long)dst, (unsigned long long)n); return; }
    vkCmdFillBuffer(G.cur, db, off, n & ~3ull, value);     // Vulkan fills whole words; a tail of 1-3 bytes would need a read-modify-write
    barrier_transfer(G.cur);
}

void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { nvn_vk_post_event(val, mode, v); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { nvn_vk_post_counter(addr, type); }
