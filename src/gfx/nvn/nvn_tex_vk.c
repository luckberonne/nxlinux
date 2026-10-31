// Textures on Vulkan: the image behind each NVN texture, its upload from the game's storage, buffer <-> texture copies and CPU reads.
//
// Where the truth is, as in nxnative's Metal layer, but tracked per subresource (a mip level of one array layer). A texture's storage lives in a
// memory pool in the Tegra's block-linear layout. For packaged textures and for what the CPU writes, that storage is the truth and the image is a
// cache of it: DIRTY says the storage holds something the image does not yet, and the image is filled (de-swizzled on the CPU) the next time
// the GPU uses it. Once the GPU writes a subresource (a buffer->texture copy; later, a render target) the image is the truth: GPU. A CPU read of a
// subresource the GPU did not write reads the storage directly; of one it did, reads the image back. Per subresource, a CPU write to one level
// does not re-upload the storage over what the GPU wrote to another, which nxnative avoids by mirroring every copy back into the storage.
//
// Images stay in VK_IMAGE_LAYOUT_GENERAL: one layout serves transfers, sampling, storage and attachments, so no transitions are tracked.
// That costs some GPU speed (compression is off for such images) and is the simple choice until draws exist to measure.
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_vk.h"
#include "nvn_vk_priv.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_fmt_vk.h"
#include "../tex/astc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NB (1u << 14)
typedef struct vktex {
    struct vktex *next;
    uint64_t serial;
    nvn_texdesc d; nvn_texlayout L; const uint8_t *mem;       // the description and storage at TextureInitialize
    int bw, bh, bpb, is3d;
    VkFormat fmt; VkImageAspectFlags aspect;
    VkImage img; VkDeviceMemory vmem;
    VkImage alias_img; VkDeviceMemory alias_vmem; int has_alias;   // for compressed textures written through an integer view (BC3 as R32G32B32A32_UINT): a second image sharing the storage, with a compatible format
    int alias_dirty;                                            // the alias was written; the compressed image needs re-uploading before sampling
    uint32_t layers, levels;
    int ok;                                                    // an image exists and the format maps
    int astc;                                                  // an ASTC texture on a device without ASTC: the image is RGBA8 and upload decodes the blocks (tex/astc.c)
    int inited;                                                // moved out of UNDEFINED into GENERAL in a recorded submission
    uint32_t nsub;                                             // subresources per level: the layers, or 1 for a 3D texture
    uint8_t *st;                                               // levels * nsub bytes of F_DIRTY | F_GPU, accessed atomically
    struct { VkFormat fmt; unsigned level, layer0, n; VkImageView v; } views[16]; int nviews;   // attachment views made so far; touched by the submit thread only
    struct { VkFormat fmt; uint32_t l0, nl, a0, na; int vtype; uint32_t swz; VkImageView v; } svs[8]; int nsvs;   // sampling views, the same way
    uint64_t hash, hash_ms;                                    // the storage's hash and when it was taken (recheck below)
    int zero;                                                  // the storage was all zero when it was first read: the title may fill it later (a course's textures load after the objects exist), so it is watched
    int dead;                                                  // finalized: waits in g_dead to be freed
    uint64_t last_use;                                         // when a replay last attached or sampled it (g_use_tick)
    int used;                                                  // touched at least once since it died
    struct vktex *alias_of; int nalias;                        // a texture object over the same storage and description as another shares its image: alias_of names it; nalias counts the objects that name this one
    struct vktex *snext;                                       // the chain of images that can be shared (g_share)
} vktex_t;
enum { F_DIRTY = 1, F_GPU = 2 };
#define SUB(e, lv, ly) ((e)->st[(size_t)(lv) * (e)->nsub + (ly)])

static vktex_t *g_bucket[NB];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void red_once(int *flag, const char *what, int fmt) {
    if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] texture: %s (NVN format %#x)\n", what, fmt);
}

// A texture the host swept keeps a tombstone: what TextureInitialize was given (serial, description, layout, storage). A command that names the serial later
// finds the tombstone and the texture is made again, as the hardware would have it: the object is only a description of memory the game still owns, and a
// title may keep drawing into a target after finalizing it. Its contents are the storage's (dirty, uploaded at first use) or, for a target, whatever is drawn next.
enum { TOMB_MAX = 131072 };
typedef struct tomb { struct tomb *next; uint64_t serial; nvn_texdesc d; nvn_texlayout L; const uint8_t *mem; unsigned slot; } tomb_t;
static tomb_t *g_tbucket[NB], *g_tring[TOMB_MAX]; static unsigned g_tnext; static _Atomic uint64_t g_nrevived;
static void tomb_unlink(tomb_t *t) {                               // caller holds g_mu
    for (tomb_t **p = &g_tbucket[t->serial & (NB - 1)]; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    g_tring[t->slot] = NULL;
}
static void tomb_add(const vktex_t *e) {                           // caller holds g_mu
    unsigned slot = g_tnext++ % TOMB_MAX;
    if (g_tring[slot]) { tomb_t *old = g_tring[slot]; tomb_unlink(old); free(old); }   // the oldest tombstone goes when the table is full
    tomb_t *t = malloc(sizeof *t); if (!t) return;
    t->serial = e->serial; t->d = e->d; t->L = e->L; t->mem = e->mem; t->slot = slot;
    t->next = g_tbucket[t->serial & (NB - 1)]; g_tbucket[t->serial & (NB - 1)] = t; g_tring[slot] = t;
}
static tomb_t *tomb_take(uint64_t serial) {                        // removes and returns the tombstone of a serial; caller holds g_mu
    tomb_t *t = g_tbucket[serial & (NB - 1)];
    while (t && t->serial != serial) t = t->next;
    if (t) tomb_unlink(t);
    return t;
}
void mtl_tex_init(const tex_t *t);
static void dead_push(vktex_t *e);
// Texture objects over the same storage with the same description are one texture: the title draws a shadow map into one object and reads it through another it made over the same memory (the
// transient targets of Mario Kart 8 Deluxe, made anew every frame), and on the hardware both are the same bytes. The host keeps one image for them; the second object is an alias of the first.
static vktex_t *g_share[NB];
static unsigned share_slot(const uint8_t *mem) { return (unsigned)(((uintptr_t)mem >> 6) * 0x9E3779B1u >> 7) & (NB - 1); }
static int same_texture(const vktex_t *e, const nvn_texdesc *d, const nvn_texlayout *L, const uint8_t *mem) {
    return e->mem == mem && e->L.size == L->size && e->d.target == d->target && e->d.format == d->format && e->d.flags == d->flags && e->d.samples == d->samples && e->d.width == d->width
        && e->d.height == d->height && e->d.depth == d->depth && e->d.levels == d->levels && e->d.stride == d->stride;
}
static vktex_t *share_find(const nvn_texdesc *d, const nvn_texlayout *L, const uint8_t *mem) {      // caller holds g_mu
    if (!mem) return NULL;
    for (vktex_t *e = g_share[share_slot(mem)]; e; e = e->snext) if (same_texture(e, d, L, mem)) return e;
    return NULL;
}
static void share_add(vktex_t *e) { unsigned k = share_slot(e->mem); e->snext = g_share[k]; g_share[k] = e; }         // caller holds g_mu
static void share_remove(vktex_t *e) {                                                                                  // caller holds g_mu
    for (vktex_t **p = &g_share[share_slot(e->mem)]; *p; p = &(*p)->snext) if (*p == e) { *p = e->snext; break; }
}
static vktex_t *find_raw(uint64_t serial) {        // the entry of a serial as it is (an alias stays an alias); caller holds g_mu
    vktex_t *e = g_bucket[serial & (NB - 1)];
    while (e && e->serial != serial) e = e->next;
    return e;
}
static vktex_t *find(uint64_t serial) {
    pthread_mutex_lock(&g_mu);
    vktex_t *e = g_bucket[serial & (NB - 1)];
    while (e && e->serial != serial) e = e->next;
    if (e && e->alias_of) e = e->alias_of;                          // an alias is its image's texture
    tomb_t *tb = e ? NULL : tomb_take(serial);
    pthread_mutex_unlock(&g_mu);
    if (!tb) return e;
    tex_t t; memset(&t, 0, sizeof t); t.serial = tb->serial; t.d = tb->d; t.L = tb->L; t.mem = (uint8_t *)tb->mem;
    free(tb);
    mtl_tex_init(&t);                                              // inserts the new entry
    pthread_mutex_lock(&g_mu);
    e = g_bucket[serial & (NB - 1)];
    while (e && e->serial != serial) e = e->next;
    if (e) dead_push(e);                                           // it stands for a finalized object: it may be swept again, and come back again
    pthread_mutex_unlock(&g_mu);
    if (atomic_fetch_add(&g_nrevived, 1) == 0) fprintf(stderr, "[dm] [nvn] a swept texture (serial %llu) was named again and was made again from its tombstone\n", (unsigned long long)serial);
    return e;
}

// A view of an ASTC texture the device cannot hold as ASTC names the RGBA8 format the image has (sRGB or not as the requested ASTC format is)
static VkFormat vf_fix(const vktex_t *e, VkFormat f) {
    if (e->astc && f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return f;
}
static VkFormatFeatureFlags features(vkctx_t *c, VkFormat f) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(c->pd, f, &p); return p.optimalTilingFeatures; }

// A finalized texture stays findable: an NVN texture is a description of pool memory, and a title may Finalize an object (or a copy of it: they are plain
// memory) while the commands that use it keep being replayed (Mario Kart 8 Deluxe finalizes its scene targets right after making them, and makes other
// textures over the same memory). The host frees finalized textures least recently used first, once they hold more than DEAD_BYTES or number DEAD_MAX.
enum { DEAD_MAX = 32768 };                              // entries, not bytes: a course load finalizes thousands of textures at once, and a small cap evicted some before their first use (skipped draws)
static uint64_t dead_budget(void) {   // VRAM budget for dead-but-still-referenced textures; the dGPU has 4 GB, and the iGPU shares system RAM. NX_DEAD_MB overrides it
    static uint64_t b; if (!b) { const char *e = getenv("NX_DEAD_MB"); b = (e && atol(e) > 0 ? (uint64_t)atol(e) : 1536ull) << 20; }
    return b;
}
#define DEAD_BYTES dead_budget()
static struct dead { vktex_t *e; } *g_dead; static size_t g_ndead, g_cdead; static uint64_t g_dead_bytes;
static uint64_t g_nswept;
static _Atomic uint64_t g_use_tick;
static void touch(vktex_t *e) { if (e) { e->last_use = atomic_fetch_add(&g_use_tick, 1) + 1; e->used = 1; } }
// The serials that recently left the table and why: when the game attaches one of them the log says how it went instead of a bare "never registered".
// (A render target that is finalized or re-initialized while replayed commands still name its serial is a skipped draw.)
enum { GONE_MAX = 8192, GONE_FINALIZED = 1, GONE_REINIT = 2, GONE_SWEPT = 3 };
static struct { uint64_t serial; uint64_t tick; uint8_t why; } g_gone[GONE_MAX];
static unsigned g_ngone_total;
static void gone_add(uint64_t serial, int why) {
    if (!serial) return;
    unsigned i = g_ngone_total++ % GONE_MAX;
    g_gone[i].serial = serial; g_gone[i].tick = atomic_load(&g_use_tick); g_gone[i].why = (uint8_t)why;
}
static int gone_why(uint64_t serial, uint64_t *age) {              // the reason and how many texture uses ago (0 when the serial is not in the ring)
    unsigned n = g_ngone_total < GONE_MAX ? g_ngone_total : GONE_MAX;
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (g_ngone_total - 1 - k) % GONE_MAX;
        if (g_gone[i].serial == serial) { if (age) *age = atomic_load(&g_use_tick) - g_gone[i].tick; return g_gone[i].why; }
    }
    return 0;
}
static void free_tex(vktex_t *e) {
    for (int i = 0; i < e->nviews; i++) nvn_vk_defer_image(VK_NULL_HANDLE, e->views[i].v, VK_NULL_HANDLE);
    for (int i = 0; i < e->nsvs; i++) nvn_vk_defer_image(VK_NULL_HANDLE, e->svs[i].v, VK_NULL_HANDLE);
    if (e->img) nvn_vk_defer_image(e->img, VK_NULL_HANDLE, e->vmem);           // a submission in flight may still use it
    if (e->has_alias) { vkctx_t *_c = nvn_vk_ctx(); if (_c) { vkDestroyImage(_c->dev, e->alias_img, NULL); vkFreeMemory(_c->dev, e->alias_vmem, NULL); } }
    free(e->st); free(e);
}
static void dead_push(vktex_t *e) {                                // caller holds g_mu
    e->dead = 1;
    if (g_ndead == g_cdead) { g_cdead = g_cdead ? g_cdead * 2 : 256; g_dead = realloc(g_dead, g_cdead * sizeof *g_dead); }
    g_dead[g_ndead++] = (struct dead){e}; g_dead_bytes += e->alias_of ? 0 : e->L.size;
}
typedef struct { uint64_t key; size_t idx; } evict_t;
static int evict_cmp(const void *a, const void *b) { uint64_t x = ((const evict_t *)a)->key, y = ((const evict_t *)b)->key; return x < y ? -1 : x > y; }
// Over its budget the dead pool is brought down to nine tenths of it in one pass: the entries are put in eviction order once (the ones that were used before the never-used, the oldest first; an
// image another texture object still names stays) instead of searching the whole list again for every texture that goes
static void sweep_dead(void) {                                    // caller holds g_mu
    if (!(g_ndead > DEAD_MAX || g_dead_bytes > DEAD_BYTES)) return;
    uint64_t want_bytes = DEAD_BYTES - DEAD_BYTES / 10; size_t want_n = DEAD_MAX - DEAD_MAX / 10;
    evict_t *cand = malloc(g_ndead * sizeof *cand); if (!cand) return;
    size_t nc = 0;
    for (size_t i = 0; i < g_ndead; i++) {
        const vktex_t *e = g_dead[i].e; if (e->nalias) continue;
        cand[nc].key = (e->used ? 0ull : 1ull << 62) | e->last_use; cand[nc++].idx = i;
    }
    qsort(cand, nc, sizeof *cand, evict_cmp);
    unsigned char *drop = calloc(g_ndead, 1); vktex_t **gone = malloc((nc ? nc : 1) * sizeof *gone); size_t ngone = 0, left = g_ndead; uint64_t bytes = g_dead_bytes;
    for (size_t k = 0; k < nc && drop && gone && (bytes > want_bytes || left > want_n); k++) {
        vktex_t *e = g_dead[cand[k].idx].e; drop[cand[k].idx] = 1; left--; bytes -= e->alias_of ? 0 : e->L.size;
        for (vktex_t **p = &g_bucket[e->serial & (NB - 1)]; *p; p = &(*p)->next) if (*p == e) { *p = e->next; break; }
        if (e->alias_of) e->alias_of->nalias--; else share_remove(e);
        gone_add(e->serial, GONE_SWEPT); g_nswept++; tomb_add(e);
        gone[ngone++] = e;
    }
    if (drop && ngone) { size_t w = 0; for (size_t i = 0; i < g_ndead; i++) if (!drop[i]) g_dead[w++] = g_dead[i]; g_ndead = w; g_dead_bytes = bytes; }
    free(cand); free(drop);
    if (!ngone) { free(gone); return; }
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < ngone; i++) free_tex(gone[i]);
    free(gone);
    pthread_mutex_lock(&g_mu);
}
static long tex_trace(void) { static long s = -2; if (s == -2) s = getenv("NX_TEX_TRACE") ? atol(getenv("NX_TEX_TRACE")) : -1; return s; }   // NX_TEX_TRACE=<serial>: its creation and release
void mtl_tex_init(const tex_t *t) {
    vkctx_t *c = nvn_vk_ctx();
    if (tex_trace() >= 0 && (uint64_t)tex_trace() == t->serial) fprintf(stderr, "[dm] [nvn] trace: texture serial %llu initialized %ux%ux%u fmt %#x target %d flags %#x (vulkan %s)\n", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.flags, c ? "up" : "not up");
    if (!c || !t->serial) return;
    static int bad_fmt, bad_ms, bad_img;
    vktex_t *e = calloc(1, sizeof *e);
    e->serial = t->serial; e->d = t->d; e->L = t->L; e->mem = t->mem;
    {
        static int off = -1; if (off < 0) off = getenv("NX_NO_TEX_SHARE") != NULL;       // NX_NO_TEX_SHARE=1: one image per texture object, as before
        pthread_mutex_lock(&g_mu);
        vktex_t *pr = off ? NULL : share_find(&t->d, &t->L, t->mem);
        if (pr) { e->alias_of = pr; pr->nalias++; e->next = g_bucket[e->serial & (NB - 1)]; g_bucket[e->serial & (NB - 1)] = e; }
        pthread_mutex_unlock(&g_mu);
        if (pr) { static _Atomic uint64_t n; if (atomic_fetch_add(&n, 1) == 0) fprintf(stderr, "[dm] [nvn] texture sharing: serial %llu is made over the storage and description of serial %llu and shares its image\n", (unsigned long long)t->serial, (unsigned long long)pr->serial); return; }
    }
    e->bpb = nvn_fmt_bpb(t->d.format, &e->bw, &e->bh);
    e->is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    e->layers = (t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ||
                 t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) && t->L.layers ? t->L.layers : 1;
    uint32_t dim = t->d.width > t->d.height ? t->d.width : t->d.height; if (e->is3d && t->d.depth > dim) dim = t->d.depth;
    uint32_t maxl = 1; while ((dim >> maxl) && maxl < 16) maxl++;
    e->levels = t->d.levels ? (t->d.levels < maxl ? t->d.levels : maxl) : 1;
    e->nsub = e->is3d ? 1 : e->layers;
    e->st = calloc((size_t)e->levels * e->nsub, 1);
    if (e->mem && !getenv("NX_TEX_CLEAN")) memset(e->st, F_DIRTY, (size_t)e->levels * e->nsub);       // the storage is the truth until the GPU writes the texture: a title that fills it from the CPU after creating it (no packaged data, no flush) gets it uploaded at first use, as nxnative does
    int kind = nvn_fmt_to_vk(t->d.format, &e->fmt, NULL);
    if (kind && t->d.format >= 0x79 && t->d.format <= 0x94 && !nvn_fmt_vk_supported(c->pd, e->fmt)) {    // ASTC, which this GPU lacks: RGBA8, decoded on upload
        e->astc = 1; e->fmt = t->d.format < 0x87 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
    }
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) goto keep;                                           // buffer textures are views of pool memory, not images
    if (t->d.samples > 1) { red_once(&bad_ms, "multisampled textures are not supported yet", t->d.format); goto keep; }
    if (!kind || (!e->astc && !nvn_fmt_vk_supported(c->pd, e->fmt))) { red_once(&bad_fmt, kind ? "the format has no Vulkan support on this device (ASTC and similar need a decoder)" : "the format has no Vulkan counterpart (3-component formats need expanding)", t->d.format); goto keep; }
    e->aspect = e->fmt == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : e->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
              : (e->fmt == VK_FORMAT_D16_UNORM || e->fmt == VK_FORMAT_D32_SFLOAT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    {
        VkFormatFeatureFlags f = features(c, e->fmt);
        VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) u |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        int cube = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube || (!e->is3d && e->layers >= 6 && t->d.width == t->d.height && t->d.target != NVN_TEXTURE_TARGET_1D_ARRAY) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0)   // a square 2D array of 6 layers or more may be sampled as a cube (the descriptor, not the object, says which)
                   | (e->is3d && (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),   // a slice of a 3D texture can be a render target (colour LUTs)
            .imageType = e->is3d ? VK_IMAGE_TYPE_3D : t->d.target == NVN_TEXTURE_TARGET_1D || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
            .format = e->fmt, .extent = {t->d.width ? t->d.width : 1, t->d.height ? t->d.height : 1, e->is3d && t->d.depth ? t->d.depth : 1},
            .mipLevels = e->levels, .arrayLayers = e->layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (ii.imageType == VK_IMAGE_TYPE_1D) ii.extent.height = 1;
        VkMemoryRequirements mr;
        if (vkCreateImage(c->dev, &ii, NULL, &e->img)) { red_once(&bad_img, "vkCreateImage failed", t->d.format); e->img = VK_NULL_HANDLE; goto keep; }
        vkGetImageMemoryRequirements(c->dev, e->img, &mr);
        uint32_t mt = vkctx_memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
        if (mt == UINT32_MAX || vkAllocateMemory(c->dev, &ai, NULL, &e->vmem) || vkBindImageMemory(c->dev, e->img, e->vmem, 0)) {
            red_once(&bad_img, "no memory for the image", t->d.format); vkDestroyImage(c->dev, e->img, NULL); e->img = VK_NULL_HANDLE; e->vmem = VK_NULL_HANDLE; goto keep;
        }
        e->ok = 1;
    }
keep:
    pthread_mutex_lock(&g_mu);
    e->next = g_bucket[e->serial & (NB - 1)]; g_bucket[e->serial & (NB - 1)] = e;      // a serial is unique: a re-initialised object gets a new one
    if (e->ok && e->mem && e->d.samples <= 1 && e->d.target != NVN_TEXTURE_TARGET_BUFFER) share_add(e);
    pthread_mutex_unlock(&g_mu);
}

// How many textures the host holds and how much of that is only kept for commands that may still name it (the timing line reports it)
void mtl_tex_stats(uint64_t *live, uint64_t *dead, uint64_t *dead_bytes, uint64_t *swept) {
    uint64_t n = 0;
    pthread_mutex_lock(&g_mu);
    for (size_t b = 0; b < NB; b++) for (vktex_t *e = g_bucket[b]; e; e = e->next) if (!e->dead) n++;
    *live = n; *dead = g_ndead; *dead_bytes = g_dead_bytes; *swept = g_nswept;
    pthread_mutex_unlock(&g_mu);
}
void mtl_tex_release(tex_t *t) {
    if (tex_trace() >= 0 && (uint64_t)tex_trace() == t->serial) fprintf(stderr, "[dm] [nvn] trace: texture serial %llu finalized\n", (unsigned long long)t->serial);
    pthread_mutex_lock(&g_mu);
    vktex_t *e = g_bucket[t->serial & (NB - 1)];
    while (e && e->serial != t->serial) e = e->next;
    if (e && !e->dead) {
        gone_add(t->serial, GONE_FINALIZED);
        dead_push(e);
    }
    sweep_dead();
    pthread_mutex_unlock(&g_mu);
}
void mtl_tex_reinit(tex_t *t) {   // TextureInitialize on a live object: the old serial leaves the table the same way, but kept findable so commands still naming it resolve
    gone_add(t->serial, GONE_REINIT);
    mtl_tex_release(t);
}
void mtl_tex_cpu_written_sub(const tex_t *t, unsigned level, unsigned layer0, unsigned nlayers) {
    vktex_t *e = find(t->serial);
    if (!e || level >= e->levels) return;
    for (unsigned ly = layer0; ly < layer0 + nlayers && ly < e->nsub; ly++) {
        uint8_t *f = &SUB(e, level, ly);
        __atomic_store_n(f, (uint8_t)((__atomic_load_n(f, __ATOMIC_RELAXED) & ~F_GPU) | F_DIRTY), __ATOMIC_RELEASE);
    }
}
void mtl_tex_cpu_written(const tex_t *t) {                       // the whole texture, when the caller does not know what it wrote
    vktex_t *e = find(t->serial);
    if (e) for (unsigned lv = 0; lv < e->levels; lv++) mtl_tex_cpu_written_sub(t, lv, 0, e->nsub);
}
int mtl_tex_gpu_owned(uint64_t serial) {
    vktex_t *e = find(serial);
    if (e) for (size_t i = 0; i < (size_t)e->levels * e->nsub; i++) if (__atomic_load_n(&e->st[i], __ATOMIC_ACQUIRE) & F_GPU) return 1;
    return 0;
}

// ---- using a texture in the submission being recorded
static void level_dims(const vktex_t *e, unsigned level, uint32_t *w, uint32_t *h, uint32_t *z) {
    *w = e->d.width >> level ? e->d.width >> level : 1; *h = e->d.height >> level ? e->d.height >> level : 1;
    *z = e->is3d ? (e->d.depth >> level ? e->d.depth >> level : 1) : 1;
}
static void to_general(vktex_t *e, VkCommandBuffer cb) {
    if (e->inited) return;
    vk_barrier_image_all(cb, e->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, e->aspect);   // every level and every layer
    e->inited = 1;
    // A new depth or stencil image reads as zero, as a freshly mapped Switch memory pool does (a title may test against a depth texture it never clears: Vulkan would give undefined contents)
    if (e->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        VkClearDepthStencilValue z = {0.0f, 0};
        VkImageSubresourceRange rg = {e->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        nvn_vk_barrier_all(cb);
    }
}

// De-swizzle the dirty subresources of the storage and copy them into the image
static int bc_block_size(VkFormat f);
static void upload(vktex_t *e, vkctx_t *c, VkCommandBuffer cb) {
    static int skipped;
    if (!e->mem) return;
    if (e->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) { red_once(&skipped, "uploading a depth-stencil texture from storage is not supported yet", e->d.format); return; }
    if (e->d.samples > 1) return;
    if (e->zero && !e->astc && !bc_block_size(e->fmt) && (e->aspect == VK_IMAGE_ASPECT_COLOR_BIT || e->aspect == VK_IMAGE_ASPECT_DEPTH_BIT)) {
        // The storage held nothing when it was first looked at (a render target made over fresh pool memory): its dirty subresources are cleared to zero, which is what the upload would write
        int any = 0;
        for (unsigned lv = 0; lv < e->levels; lv++) for (unsigned ly = 0; ly < e->nsub; ly++) {
            if (!(__atomic_fetch_and(&SUB(e, lv, ly), (uint8_t)~F_DIRTY, __ATOMIC_ACQ_REL) & F_DIRTY)) continue;
            VkImageSubresourceRange rg = {e->aspect, lv, 1, ly, 1};
            if (e->aspect == VK_IMAGE_ASPECT_COLOR_BIT) { VkClearColorValue z = {{0}}; vkCmdClearColorImage(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
            else { VkClearDepthStencilValue z = {0.0f, 0}; vkCmdClearDepthStencilImage(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
            any = 1;
        }
        if (any) nvn_vk_barrier_all(cb);
        return;
    }
    size_t nreg = (size_t)e->levels * e->nsub, total = 0, n = 0;
    VkBufferImageCopy *rg = calloc(nreg ? nreg : 1, sizeof *rg);
    size_t *off = calloc(nreg ? nreg : 1, sizeof *off), *pitch = calloc(nreg ? nreg : 1, sizeof *pitch);
    unsigned *slv = calloc(nreg ? nreg : 1, sizeof *slv), *sly = calloc(nreg ? nreg : 1, sizeof *sly);
    for (unsigned lv = 0; lv < e->levels; lv++) {
        uint32_t w, h, z; level_dims(e, lv, &w, &h, &z);
        int is3comp = (e->bpb == 3 || e->bpb == 6 || e->bpb == 12);
        size_t wb = is3comp ? (size_t)w * (e->bpb == 3 ? 4 : (e->bpb == 6 ? 8 : 16))  // expand 3->4 bytes per pixel
            : (size_t)((w + e->bw - 1) / e->bw) * e->bpb;
        size_t hb = (h + e->bh - 1) / e->bh;
        for (unsigned ly = 0; ly < e->nsub; ly++) {
            if (!(__atomic_fetch_and(&SUB(e, lv, ly), (uint8_t)~F_DIRTY, __ATOMIC_ACQ_REL) & F_DIRTY)) continue;     // clean: the image is current
            total = (total + 15) & ~(size_t)15;
            off[n] = total; pitch[n] = wb; slv[n] = lv; sly[n] = ly; total += e->astc ? (size_t)w * h * z * 4 : wb * hb * z;     // an ASTC level is stored decoded
            rg[n] = (VkBufferImageCopy){.bufferOffset = off[n], .imageSubresource = {e->aspect, lv, ly, 1}, .imageExtent = {w, h, z}};
            n++;
        }
    }
    vkbuf_t st;
    if (!n || !total || vkbuf_create(c, &st, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1)) goto done;
    if (e->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {      // already linear: rows at d.stride, level 0 only
        uint32_t w, h, z; level_dims(e, 0, &w, &h, &z);
        int is3comp = (e->bpb == 3 || e->bpb == 6 || e->bpb == 12);
        size_t wb3 = (size_t)((w + e->bw - 1) / e->bw) * e->bpb, hb = (h + e->bh - 1) / e->bh, stride = e->d.stride ? e->d.stride : wb3;
        size_t wb4 = is3comp ? (size_t)w * (e->bpb == 3 ? 4 : (e->bpb == 6 ? 8 : 16)) : wb3;
        if (is3comp) {
            for (size_t y = 0; y < hb; y++) {
                uint8_t *src = (uint8_t *)e->mem + y * stride;
                uint8_t *dst = (uint8_t *)st.map + off[0] + y * wb4;
                for (size_t x = 0; x < w; x++) {
                    memcpy(dst + x * 4, src + x * 3, 3);
                    dst[x * 4 + 3] = 0xFF;
                }
            }
        } else {
            for (size_t y = 0; y < hb; y++) memcpy((uint8_t *)st.map + off[0] + y * wb3, e->mem + y * stride, wb3);
        }
        n = 1;
    } else {
        for (size_t i = 0; i < n; i++) {
            if (e->astc) {                      // de-swizzle the blocks, then decode them into the RGBA8 staging
                uint32_t w, h, z; level_dims(e, slv[i], &w, &h, &z);
                size_t hb = (h + e->bh - 1) / e->bh, bytes = pitch[i] * hb * z;
                uint8_t *tmp = calloc(bytes ? bytes : 1, 1);
                if (nvn_deswizzle(&e->d, slv[i], sly[i], e->mem, tmp, pitch[i])) memset(tmp, 0, bytes);
                for (uint32_t zz = 0; zz < z; zz++) astc_decode_image(tmp + zz * pitch[i] * hb, (int)w, (int)h, e->bw, e->bh, (uint8_t *)st.map + off[i] + (size_t)zz * w * h * 4);
                free(tmp);
            } else {
                int is3comp = (e->bpb == 3 || e->bpb == 6 || e->bpb == 12);
                if (is3comp) {
                    uint32_t w, h, z; level_dims(e, slv[i], &w, &h, &z);
                    size_t hb = (h + e->bh - 1) / e->bh;
                    size_t wb3 = (size_t)w * e->bpb;  // 3 bytes per pixel (source)
                    size_t tmp_sz = wb3 * hb * z ? wb3 * hb * z : 1;
                    uint8_t *tmp = calloc(tmp_sz, 1);
                    if (nvn_deswizzle(&e->d, slv[i], sly[i], e->mem, tmp, wb3)) {
                        static _Atomic int nfail; if (atomic_fetch_add(&nfail, 1) < 8) fprintf(stderr, "[dm] [RED] [nvn] texture upload: de-swizzle of level %u layer %u failed (%ux%ux%u, NVN format %#x, target %d, flags %#x, stride %u): the level stays zero\n", slv[i], sly[i], e->d.width, e->d.height, e->d.depth, e->d.format, e->d.target, e->d.flags, e->d.stride);
                        memset((uint8_t *)st.map + off[i], 0, pitch[i] * hb * z);
                    } else {
                        // Expand 3 bytes -> 4 bytes per pixel (RGB -> RGBA, alpha = 1)
                        size_t wb4 = pitch[i];
                        for (size_t y = 0; y < hb * z; y++) {
                            uint8_t *src = tmp + y * wb3;
                            uint8_t *dst = (uint8_t *)st.map + off[i] + y * wb4;
                            for (size_t x = 0; x < w; x++) {
                                memcpy(dst + x * 4, src + x * 3, 3);
                                dst[x * 4 + 3] = 0xFF;  // alpha = 1.0 (ignored by shader as RGBX, but set for correctness)
                            }
                        }
                    }
                    free(tmp);
                } else if (nvn_deswizzle(&e->d, slv[i], sly[i], e->mem, (uint8_t *)st.map + off[i], pitch[i])) {
                    static _Atomic int nfail; if (atomic_fetch_add(&nfail, 1) < 8) fprintf(stderr, "[dm] [RED] [nvn] texture upload: de-swizzle of level %u layer %u failed (%ux%ux%u, NVN format %#x, target %d, flags %#x, stride %u): the level stays zero\n", slv[i], sly[i], e->d.width, e->d.height, e->d.depth, e->d.format, e->d.target, e->d.flags, e->d.stride);
                    memset((uint8_t *)st.map + off[i], 0, pitch[i]);
                }
            }
        }
    }
    { static _Atomic unsigned nup; unsigned k = atomic_fetch_add(&nup, 1); if (getenv("NX_DRAW_DETAIL") && (k < 10 || k % 500 == 0)) { size_t nz = 0, ck = total < 4096 ? total : 4096; for (size_t q = 0; q < ck; q++) nz += ((uint8_t *)st.map)[q] != 0; fprintf(stderr, "[dm] [nvn] texture upload #%u: serial %llu %ux%u fmt %#x, %zu regions, %zu bytes staged (%zu of the first %zu non-zero), storage %p\n", k, (unsigned long long)e->serial, e->d.width, e->d.height, e->d.format, n, total, nz, ck, (void *)e->mem); } }
    to_general(e, cb);
    vkCmdCopyBufferToImage(cb, st.buf, e->img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)n, rg);
    nvn_vk_barrier_all(cb);
    nvn_vk_keep(&st);
done:
    free(rg); free(off); free(pitch); free(slv); free(sly);
}

// Make the image current for a GPU use in the submission being recorded; returns 0 when the texture has no image
// CPU writes into a texture's storage after its first upload (glyph caches, the text of menus) do not fault and nothing else reports them: the storage is
// hashed again at most every 100 ms when the texture is used, and a change makes every subresource the GPU has not written dirty
static uint64_t storage_hash(const uint8_t *p, size_t n, int *allzero) {
    uint64_t h = 0x9E3779B97F4A7C15ull, w, any = 0; size_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); any |= w; h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
    for (; i < n; i++) { any |= p[i]; h = (h ^ p[i]) * 0x100000001B3ull; }
    if (allzero) *allzero = any == 0;
    return h;
}
static int storage_zero(const uint8_t *p, size_t n) {              // every byte zero; stops at the first one that is not (a texture with data answers at once)
    size_t i = 0; uint64_t w;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); if (w) return 0; }
    for (; i < n; i++) if (p[i]) return 0;
    return 1;
}
static int storage_sampled_nonzero(const uint8_t *p, size_t n) {   // a few words spread over the storage: cheap enough to ask every 100 ms of a texture that was empty
    uint64_t w;
    for (size_t i = 0; i + 8 <= n; i += 4096) { memcpy(&w, p + i, 8); if (w) return 1; }
    if (n >= 8) { memcpy(&w, p + n - 8, 8); if (w) return 1; }
    return 0;
}
static void mark_cpu_dirty(vktex_t *e) {
    for (size_t i = 0; i < (size_t)e->levels * e->nsub; i++) {
        uint8_t f = __atomic_load_n(&e->st[i], __ATOMIC_ACQUIRE);
        if (!(f & F_GPU)) __atomic_store_n(&e->st[i], (uint8_t)(f | F_DIRTY), __ATOMIC_RELEASE);
    }
}
static void recheck(vktex_t *e) {
    static int off = -1; if (off < 0) off = getenv("NX_TEX_NO_RECHECK") != NULL;
    if (off || !e->mem || !e->L.size || e->L.size > (64u << 20)) return;
    int cpu = 0; for (size_t i = 0; i < (size_t)e->levels * e->nsub && !cpu; i++) cpu = !(__atomic_load_n(&e->st[i], __ATOMIC_ACQUIRE) & F_GPU);
    if (!cpu) return;                                              // a render target: the GPU owns every subresource
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    if (e->hash_ms && ms - e->hash_ms < 100) return;
    int first_look = !e->hash_ms; e->hash_ms = ms;
    if (first_look) { e->zero = storage_zero(e->mem, e->L.size); return; }       // the initial contents are uploaded by the F_DIRTY set at creation; all that is asked now is whether they are empty
    if (e->zero) {                                                 // empty when first looked at: has the title filled it since?
        if (!storage_sampled_nonzero(e->mem, e->L.size) || storage_zero(e->mem, e->L.size)) return;
        e->zero = 0; mark_cpu_dirty(e); return;
    }
    int z = 0; uint64_t h = storage_hash(e->mem, e->L.size, &z);
    if (h == e->hash) return;
    int first = !e->hash; e->hash = h;
    if (first) return;                                             // the first hash is only the baseline
    mark_cpu_dirty(e);
}
static int use(vktex_t *e, vkctx_t *c, VkCommandBuffer cb) {
    if (!e || !e->ok) return 0;
    if (e->has_alias && e->alias_dirty) {   // a compressed texture written through its integer alias: copy the raw blocks back into the compressed image (both are linear-tiled with the same row pitch, so a buffer round-trip moves the bytes unchanged)
        uint32_t aw = (e->d.width + 3) / 4; if (!aw) aw = 1;
        uint32_t ah = (e->d.height + 3) / 4; if (!ah) ah = 1;
        size_t total = (size_t)aw * ah * 16;
        vkbuf_t st;
        if (!vkbuf_create(c, &st, total ? total : 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) {
            VkBufferImageCopy r1 = { .bufferOffset = 0, .bufferRowLength = aw, .bufferImageHeight = ah, .imageSubresource = {e->aspect, 0, 0, 1}, .imageOffset = {0, 0, 0}, .imageExtent = {aw, ah, 1} };
            VkBufferImageCopy r2 = { .bufferOffset = 0, .bufferRowLength = aw, .bufferImageHeight = ah, .imageSubresource = {e->aspect, 0, 0, 1}, .imageOffset = {0, 0, 0}, .imageExtent = {aw, ah, 1} };
            vkCmdCopyImageToBuffer(cb, e->alias_img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &r1);
            vkCmdCopyBufferToImage(cb, st.buf, e->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r2);
            nvn_vk_keep(&st);
        }
        e->alias_dirty = 0;
    }
    recheck(e);
    to_general(e, cb);
    upload(e, c, cb);                                              // only what is dirty
    return 1;
}

// ---- buffer <-> texture copies
// One region per copy: the level, the first layer and the buffer layout come from the record and the stride state
// A buffer copy takes one aspect: the depth of a depth-stencil texture (its stencil is not copied)
static VkImageAspectFlags copy_aspect(const vktex_t *e) { return (e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : e->aspect; }
static int regions(const vktex_t *e, const p_copy_b2t *c, uint64_t row, uint64_t img, VkBuffer *buf, VkBufferImageCopy *out, const char *what) {
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    uint32_t lw, lh, lz; level_dims(e, level, &lw, &lh, &lz);
    int nz = c->r.depth ? c->r.depth : 1;
    if (level >= e->levels || c->r.xoffset < 0 || c->r.yoffset < 0 || c->r.zoffset < 0 || c->r.width <= 0 || c->r.height <= 0 ||
        (uint32_t)(c->r.xoffset + c->r.width) > lw || (uint32_t)(c->r.yoffset + c->r.height) > lh ||
        (e->is3d ? (uint32_t)(c->r.zoffset + nz) > lz : (unsigned)(layer0 + c->r.zoffset + nz) > e->layers)) {
        fprintf(stderr, "[dm] [RED] [nvn] %s: region %d,%d,%d %dx%dx%d level %u is outside the texture (%ux%ux%u, %u levels, %u layers)\n", what,
                c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, nz, level, lw, lh, lz, e->levels, e->layers);
        return 0;
    }
    if (e->astc) { fprintf(stderr, "[dm] [RED] [nvn] %s: copying compressed data into an ASTC texture the device holds decoded is not supported\n", what); return 0; }
    size_t wb = (size_t)((c->r.width + e->bw - 1) / e->bw) * e->bpb, hb = (c->r.height + e->bh - 1) / e->bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    if (row % e->bpb || img % row || row < wb) { fprintf(stderr, "[dm] [RED] [nvn] %s: row stride %llu / image stride %llu do not fit %d-byte blocks\n", what, (unsigned long long)row, (unsigned long long)img, e->bpb); return 0; }
    VkDeviceSize boff;
    if (!nvn_vk_pool_buffer(c->src, img * (uint64_t)nz, buf, &boff)) { fprintf(stderr, "[dm] [RED] [nvn] %s: the buffer address %#llx is in no memory pool\n", what, (unsigned long long)c->src); return 0; }
    if (boff % e->bpb || boff % 4) { fprintf(stderr, "[dm] [RED] [nvn] %s: buffer offset %llu is not aligned to the texel block\n", what, (unsigned long long)boff); return 0; }
    *out = (VkBufferImageCopy){.bufferOffset = boff, .bufferRowLength = (uint32_t)(row / e->bpb) * e->bw, .bufferImageHeight = (uint32_t)(img / row) * e->bh,
        .imageSubresource = {copy_aspect(e), level, e->is3d ? 0 : layer0 + c->r.zoffset, e->is3d ? 1u : (uint32_t)nz},
        .imageOffset = {c->r.xoffset, c->r.yoffset, e->is3d ? c->r.zoffset : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, e->is3d ? (uint32_t)nz : 1}};
    return 1;
}

void nvn_vk_copy_b2t(const p_copy_b2t *c, uint64_t row, uint64_t img) {
    vkctx_t *ctx = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd();
    if (!ctx || !cb || !c->tex) return;
    vktex_t *e = find(c->tex->serial);
    if (!use(e, ctx, cb)) return;                                  // uploads the storage first, so the parts the copy leaves alone are right
    VkBuffer buf; VkBufferImageCopy rg;
    if (!regions(e, c, row, img, &buf, &rg, "buffer->texture copy")) return;
    vkCmdCopyBufferToImage(cb, buf, e->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    nvn_vk_barrier_all(cb);
    unsigned lv = rg.imageSubresource.mipLevel, l0 = e->is3d ? 0 : rg.imageSubresource.baseArrayLayer, nl = e->is3d ? 1 : rg.imageSubresource.layerCount;
    for (unsigned ly = l0; ly < l0 + nl && ly < e->nsub; ly++) __atomic_store_n(&SUB(e, lv, ly), F_GPU, __ATOMIC_RELEASE);   // the image holds what the storage does not
}
void nvn_vk_copy_t2b(const p_copy_b2t *c, uint64_t row, uint64_t img) {
    vkctx_t *ctx = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd();
    if (!ctx || !cb || !c->tex) return;
    vktex_t *e = find(c->tex->serial);
    if (!use(e, ctx, cb)) return;
    VkBuffer buf; VkBufferImageCopy rg;
    if (!regions(e, c, row, img, &buf, &rg, "texture->buffer copy")) return;
    vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &rg);
    nvn_vk_barrier_all(cb);
}

// ---- CPU reads (nvnTextureReadTexels)
// One layer (or the one slice group of a 3D texture) of the region: de-swizzled from the storage when the GPU did not write it, read back otherwise
static void read_storage(const tex_t *t, unsigned level, unsigned layer, int z0, int nzz, int zbase, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img, int is3d) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = (size_t)(r->yoffset / bh);
    if (!t->mem) { for (int z = 0; z < nzz; z++) memset(dst + (size_t)(zbase + z) * img, 0, img); return; }
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1, lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    if ((t->d.flags & 0x50) && !level && t->d.stride) {             // a linear texture: rows t->d.stride bytes apart, layers one after the other
        for (int z = 0; z < nzz; z++) {
            const uint8_t *src = (const uint8_t *)t->mem + (size_t)(is3d ? z0 + z : (int)layer) * t->d.stride * lr;
            for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)(zbase + z) * img + y * row, src + (y0 + y) * t->d.stride + x0, wb);
        }
        return;
    }
    uint8_t *lin = malloc(lp * lr * lz);
    if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { fprintf(stderr, "[dm] [RED] [nvn] ReadTexels: de-swizzling failed (format %#x target %d)\n", t->d.format, t->d.target); free(lin); return; }
    for (int z = 0; z < nzz; z++) {
        const uint8_t *src = lin + (is3d ? (size_t)(z0 + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)(zbase + z) * img + y * row, src + (y0 + y) * lp + x0, wb);
    }
    free(lin);
}
// Debugging: how many registered textures hold any non-zero byte in their level 0 (read as the title wrote them); prints a summary by format
long nvn_vk_debug_lit(uint64_t serial, size_t *total, const char *ppm);
void nvn_vk_debug_texstats(void) {
    {   // the texture formats the title uses, by NVN format: how many textures, how many bytes of storage, how many the host made as an image (NX_FRAME_DUMP at frame 1000)
        static struct { unsigned fmt, n, ok; unsigned long long bytes; } h[64]; unsigned nh = 0;
        pthread_mutex_lock(&g_mu);
        for (int b = 0; b < NB; b++) for (vktex_t *e = g_bucket[b]; e; e = e->next) {
            unsigned i = 0; while (i < nh && h[i].fmt != e->d.format) i++;
            if (i == nh) { if (nh == 64) continue; h[nh].fmt = e->d.format; h[nh].n = 0; h[nh].ok = 0; h[nh].bytes = 0; nh++; }
            h[i].n++; h[i].ok += e->ok; h[i].bytes += e->L.size;
        }
        pthread_mutex_unlock(&g_mu);
        for (unsigned a = 0; a < nh; a++) for (unsigned c2 = a + 1; c2 < nh; c2++) if (h[c2].bytes > h[a].bytes) { typeof(h[0]) t = h[a]; h[a] = h[c2]; h[c2] = t; }
        for (unsigned a = 0; a < nh && a < 14; a++) fprintf(stderr, "[dm] [nvn] texture formats: NVN format %#x: %u textures (%u with an image), %llu MB of storage%s\n", h[a].fmt, h[a].n, h[a].ok, h[a].bytes >> 20,
            h[a].fmt >= 0x79 && h[a].fmt <= 0x94 ? " (ASTC)" : h[a].fmt >= 0x49 && h[a].fmt <= 0x78 ? " (BC)" : "");
    }
    unsigned total = 0, empty = 0, big = 0, shown = 0; uint64_t sers[4096]; unsigned ns = 0;
    pthread_mutex_lock(&g_mu);
    for (int b = 0; b < NB; b++) for (vktex_t *e = g_bucket[b]; e && ns < 4096; e = e->next) sers[ns++] = e->serial;
    pthread_mutex_unlock(&g_mu);
    for (unsigned i = 0; i < ns; i++) {
        vktex_t *e = find(sers[i]); if (!e || e->d.width * e->d.height > 4096u * 4096u || e->d.target == NVN_TEXTURE_TARGET_3D) continue;
        size_t tot = 0; long lit = nvn_vk_debug_lit(sers[i], &tot, NULL); total++;
        if (e->d.target == NVN_TEXTURE_TARGET_2D_ARRAY && getenv("NX_TEX_ARRAYS"))   // NX_TEX_ARRAYS=1: every 2D array texture (font sheets are)
            fprintf(stderr, "[dm] [nvn] array texture serial %llu %ux%ux%u levels %u fmt %#x flags %#x layer size %#llx state %u\n", (unsigned long long)sers[i], e->d.width, e->d.height, e->d.depth,
                    e->levels, e->d.format, e->d.flags, (unsigned long long)e->L.layer_size, __atomic_load_n(&e->st[0], __ATOMIC_ACQUIRE));
        if (e->d.target == NVN_TEXTURE_TARGET_2D_ARRAY && e->d.format == 0x49 && getenv("NX_TEX_ARRAYS") && getenv("NX_FRAME_DUMP") && e->mem) {   // a BC4 sheet's layer 0, de-swizzled, raw blocks
            uint32_t bw4 = (e->d.width + 3) / 4, bh4 = (e->d.height + 3) / 4; uint8_t *lin = malloc((size_t)bw4 * bh4 * 8);
            if (!nvn_deswizzle(&e->d, 0, 0, e->mem, lin, (size_t)bw4 * 8)) { char fn[600]; snprintf(fn, sizeof fn, "%s.bc4_%llu.raw", getenv("NX_FRAME_DUMP"), (unsigned long long)sers[i]);
                FILE *o = fopen(fn, "wb"); if (o) { fwrite(lin, 1, (size_t)bw4 * bh4 * 8, o); fclose(o); } }
            free(lin);
        }
        if (e->d.flags & NVN_TEXTURE_FLAGS_LINEAR) {   // a linear texture (a glyph cache): its whole storage as the CPU sees it, and who owns level 0
            size_t nzb = 0; for (size_t q = 0; e->mem && q < e->L.size; q++) nzb += e->mem[q] != 0;
            fprintf(stderr, "[dm] [nvn] linear texture serial %llu %ux%u fmt %#x stride %u: %zu of %llu storage bytes not zero, level 0 state %u, mem %p\n", (unsigned long long)sers[i], e->d.width, e->d.height,
                    e->d.format, e->d.stride, nzb, (unsigned long long)e->L.size, __atomic_load_n(&e->st[0], __ATOMIC_ACQUIRE), (const void *)e->mem);
            const char *pre = getenv("NX_FRAME_DUMP");
            if (pre && e->mem && e->d.format == NVN_FORMAT_R8 && e->d.stride) {   // the glyph cache as a PGM
                char fn[600]; snprintf(fn, sizeof fn, "%s.linear%llu.pgm", pre, (unsigned long long)sers[i]);
                FILE *o = fopen(fn, "wb"); if (o) { fprintf(o, "P5\n%u %u\n255\n", e->d.width, e->d.height); for (uint32_t y = 0; y < e->d.height; y++) fwrite(e->mem + (size_t)y * e->d.stride, 1, e->d.width, o); fclose(o); }
            }
        }
        if (lit == 0) { empty++; if (shown < 12) { shown++; fprintf(stderr, "[dm] [nvn] empty texture serial %llu %ux%u fmt %#x flags %#x\n", (unsigned long long)sers[i], e->d.width, e->d.height, e->d.format, e->d.flags); } }
        else big++;
    }
    fprintf(stderr, "[dm] [nvn] textures read: %u, all zero: %u, with content: %u\n", total, empty, big);
}
// Debugging: the share of a texture's bytes (first mip, one layer) that are not zero, by serial; -1 when the host has no such texture
long nvn_vk_debug_lit(uint64_t serial, size_t *total, const char *ppm);
int nvn_vk_debug_info(uint64_t serial, unsigned *fmt, unsigned *w, unsigned *h, unsigned *levels, unsigned *dead) { vktex_t *e = find(serial); if (!e) return 0; *fmt = e->d.format; *w = e->d.width; *h = e->d.height; *levels = e->levels; *dead = (unsigned)e->dead; return 1; }
int nvn_vk_debug_mem(uint64_t serial, uint64_t *mem, uint64_t *size) { vktex_t *e = find(serial); if (!e) return 0; *mem = (uint64_t)(uintptr_t)e->mem; *size = e->L.size; return 1; }   // where the storage of a texture lies (the host address of its pool memory)
long nvn_vk_debug_lit(uint64_t serial, size_t *total, const char *ppm) {
    vktex_t *e = find(serial); if (!e) return -1;
    tex_t t; memset(&t, 0, sizeof t); t.serial = serial; t.d = e->d; t.L = e->L; t.mem = e->mem;
    int bw, bh, bpb = nvn_fmt_bpb(t.d.format, &bw, &bh);
    size_t wb = (size_t)((t.d.width + bw - 1) / bw) * bpb, hb = (t.d.height + bh - 1) / bh; uint8_t *buf = malloc(wb * hb);
    NVNcopyRegion r = {0, 0, 0, (int)t.d.width, (int)t.d.height, 1}; mtl_read_texels(&t, 0, 0, &r, buf, wb, wb * hb);
    size_t lit = 0; for (size_t i = 0; i < wb * hb; i++) lit += buf[i] != 0;
    if (t.d.format == NVN_FORMAT_RGB10A2) { uint32_t mx[4] = {0, 0, 0, 0}; size_t na = 0; for (size_t i = 0; i < (size_t)t.d.width * t.d.height; i++) { uint32_t px; memcpy(&px, buf + i * 4, 4); uint32_t c[4] = {px & 1023, px >> 10 & 1023, px >> 20 & 1023, px >> 30}; for (int k = 0; k < 4; k++) if (c[k] > mx[k]) mx[k] = c[k]; na += c[3] != 0; }
        fprintf(stderr, "[dm] [nvn] offscreen RGB10A2 maxima r %u g %u b %u a %u (of 1023/1023/1023/3), %zu pixels with alpha\n", mx[0], mx[1], mx[2], mx[3], na); }
    if (ppm && (t.d.format == NVN_FORMAT_RGBA8 || t.d.format == NVN_FORMAT_RGBA8_SRGB || t.d.format == NVN_FORMAT_BGRA8 || t.d.format == NVN_FORMAT_RGBA16F)) {            // a PPM of the common 8-bit and half-float formats
        FILE *o = fopen(ppm, "wb"); if (o) { fprintf(o, "P6\n%u %u\n255\n", t.d.width, t.d.height);
            for (size_t i = 0; i < (size_t)t.d.width * t.d.height; i++) { uint8_t c[3];
                if (t.d.format == NVN_FORMAT_RGBA16F) { for (int k = 0; k < 3; k++) { uint16_t h; memcpy(&h, buf + i * 8 + 2 * k, 2); int e = h >> 10 & 31; float f = e == 0 ? 0 : e == 31 ? 1e4f : (1.0f + (h & 1023) / 1024.0f) * (e >= 15 ? (float)(1u << (e - 15)) : 1.0f / (float)(1u << (15 - e))); if (h >> 15) f = -f; f = f < 0 ? 0 : f > 1 ? 1 : f; c[k] = (uint8_t)(f * 255); } }
                else { int bgr = t.d.format == NVN_FORMAT_BGRA8; c[0] = buf[i * 4 + (bgr ? 2 : 0)]; c[1] = buf[i * 4 + 1]; c[2] = buf[i * 4 + (bgr ? 0 : 2)]; }
                fwrite(c, 1, 3, o); } fclose(o); } }
    if (ppm && t.d.format == NVN_FORMAT_RGB10A2) {            // a PPM of an RGB10A2 target, to look at
        FILE *o = fopen(ppm, "wb"); if (o) { fprintf(o, "P6\n%u %u\n255\n", t.d.width, t.d.height);
            for (size_t i = 0; i < (size_t)t.d.width * t.d.height; i++) { uint32_t px; memcpy(&px, buf + i * 4, 4); uint8_t c[3] = {(uint8_t)((px & 1023) >> 2), (uint8_t)(((px >> 10) & 1023) >> 2), (uint8_t)(((px >> 20) & 1023) >> 2)}; fwrite(c, 1, 3, o); } fclose(o); } }
    free(buf); if (total) *total = wb * hb; return (long)lit;
}
// The staging buffer for reading a GPU-owned texture back (the window frame every present): kept between calls, in memory the CPU reads quickly (vkbuf_create). One reader at a time.
static pthread_mutex_t g_rb_mu = PTHREAD_MUTEX_INITIALIZER; static vkbuf_t g_rb; static size_t g_rb_size;
static vkbuf_t *rb_get(vkctx_t *c, size_t n) {                     // caller holds g_rb_mu
    if (g_rb_size >= n) return &g_rb;
    if (g_rb_size) vkbuf_destroy(c, &g_rb);
    g_rb_size = 0;
    if (vkbuf_create(c, &g_rb, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) return NULL;
    g_rb_size = n; return &g_rb;
}
// A readback that does not wait: the copy of a GPU-owned texture (the window's) into a host-cached buffer is submitted and a fence says when it is done. The caller (the presenter's thread)
// waits for the fence and reads the pixels, tightly packed; the buffer is its until it releases the job. Returns -1 when the texture is not one the GPU owns whole, and the caller reads it as usual.
static struct { vkbuf_t b; size_t cap; int busy; } g_arb[4]; static pthread_mutex_t g_arb_mu = PTHREAD_MUTEX_INITIALIZER;
int mtl_read_async_begin(const tex_t *t, const NVNcopyRegion *r, mtl_rb_t *job) {
    vkctx_t *c = nvn_vk_ctx(); vktex_t *e = find(t->serial);
    if (!c || !e || !e->ok || e->is3d || e->bpb != 4 || e->astc || !(e->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || bc_block_size(e->fmt)) return -1;
    if (!(__atomic_load_n(&SUB(e, 0, 0), __ATOMIC_ACQUIRE) & F_GPU)) return -1;
    size_t bytes = (size_t)r->width * r->height * 4; int slot = -1;
    pthread_mutex_lock(&g_arb_mu);
    for (int i = 0; i < 4 && slot < 0; i++) if (!g_arb[i].busy) {
        if (g_arb[i].cap < bytes) { if (g_arb[i].cap) vkbuf_destroy(c, &g_arb[i].b); g_arb[i].cap = 0; if (vkbuf_create(c, &g_arb[i].b, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) continue; g_arb[i].cap = bytes; }
        g_arb[i].busy = 1; slot = i;
    }
    pthread_mutex_unlock(&g_arb_mu);
    if (slot < 0) return -1;
    VkCommandBuffer cb = nvn_vk_oneshot_begin();
    if (!cb) { pthread_mutex_lock(&g_arb_mu); g_arb[slot].busy = 0; pthread_mutex_unlock(&g_arb_mu); return -1; }
    nvn_vk_barrier_all(cb);
    VkBufferImageCopy rg = {.bufferRowLength = (uint32_t)r->width, .bufferImageHeight = (uint32_t)r->height, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {r->xoffset, r->yoffset, 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
    vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, g_arb[slot].b.buf, 1, &rg);
    VkFence f = VK_NULL_HANDLE;
    if (nvn_vk_oneshot_submit(cb, &f)) { nvn_vk_oneshot_release(cb, f); pthread_mutex_lock(&g_arb_mu); g_arb[slot].busy = 0; pthread_mutex_unlock(&g_arb_mu); return -1; }
    job->slot = slot; job->cb = cb; job->fence = f; job->w = (uint32_t)r->width; job->h = (uint32_t)r->height;
    return 0;
}
const uint8_t *mtl_read_async_wait(const mtl_rb_t *job) { return nvn_vk_oneshot_wait(job->fence) ? NULL : (const uint8_t *)g_arb[job->slot].b.map; }
void mtl_read_async_done(mtl_rb_t *job) { nvn_vk_oneshot_release(job->cb, job->fence); pthread_mutex_lock(&g_arb_mu); g_arb[job->slot].busy = 0; pthread_mutex_unlock(&g_arb_mu); }

void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    vktex_t *e = find(t->serial);
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = r->depth ? r->depth : 1;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    if (is3d) {                                                              // a 3D level is one subresource
        if (e && e->ok && level < e->levels && (__atomic_load_n(&SUB(e, level, 0), __ATOMIC_ACQUIRE) & F_GPU)) goto gpu_slices;
        read_storage(t, level, 0, r->zoffset, nz, 0, r, dst, row, img, 1);
        return;
    }
    for (int z = 0; z < nz; z++) {                                           // per layer: some may be GPU-owned and some not
        unsigned layer = layer0 + (unsigned)(r->zoffset + z);
        if (e && e->ok && level < e->levels && layer < e->nsub && (__atomic_load_n(&SUB(e, level, layer), __ATOMIC_ACQUIRE) & F_GPU)) {
            NVNcopyRegion one = *r; one.zoffset = (int)(r->zoffset + z); one.depth = 1;
            mtl_finish();
            vkctx_t *ctx = nvn_vk_ctx();
            VkCommandBuffer cb = nvn_vk_oneshot_begin();
            pthread_mutex_lock(&g_rb_mu); vkbuf_t *stp = rb_get(ctx, wb * hb); vkbuf_t st; if (stp) st = *stp;
            if (!cb || !stp) { pthread_mutex_unlock(&g_rb_mu); if (cb) nvn_vk_oneshot_end(cb); memset(dst + (size_t)z * img, 0, img); continue; }
            nvn_vk_barrier_all(cb);
            VkBufferImageCopy rg = {.bufferRowLength = (uint32_t)(wb / bpb) * bw, .bufferImageHeight = (uint32_t)hb * bh,
                .imageSubresource = {copy_aspect(e), level, layer, 1}, .imageOffset = {r->xoffset, r->yoffset, 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
            vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &rg);
            nvn_vk_barrier_all(cb);
            nvn_vk_oneshot_end(cb);
            for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, (uint8_t *)st.map + y * wb, wb);
            pthread_mutex_unlock(&g_rb_mu);
        } else read_storage(t, level, layer, 0, 1, z, r, dst, row, img, 0);
    }
    return;
gpu_slices: {
        vkctx_t *ctx = nvn_vk_ctx();
        mtl_finish();
        VkCommandBuffer cb = nvn_vk_oneshot_begin();
        pthread_mutex_lock(&g_rb_mu); vkbuf_t *stp = rb_get(ctx, wb * hb * nz); vkbuf_t st; if (stp) st = *stp;
        if (!cb || !stp) { pthread_mutex_unlock(&g_rb_mu); if (cb) nvn_vk_oneshot_end(cb); memset(dst, 0, img * nz); return; }
        nvn_vk_barrier_all(cb);
        VkBufferImageCopy rg = {.bufferRowLength = (uint32_t)(wb / bpb) * bw, .bufferImageHeight = (uint32_t)hb * bh, .imageSubresource = {copy_aspect(e), level, 0, 1},
            .imageOffset = {r->xoffset, r->yoffset, r->zoffset}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, (uint32_t)nz}};
        vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &rg);
        nvn_vk_barrier_all(cb);
        nvn_vk_oneshot_end(cb);
        for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, (uint8_t *)st.map + ((size_t)z * hb + y) * wb, wb);
        pthread_mutex_unlock(&g_rb_mu);
    }
}

// ---- render targets and clears
// A view of the image to attach: format (a view may reinterpret, an RGBA8_SRGB view of an RGBA8 texture), one mip level, `n` layers from `layer0`
static VkImage view_image(vktex_t *e, VkFormat fmt) {   // the image a view with this format is created on; compressed textures that get an incompatible (integer) view use the alias
    if (e->has_alias && fmt != e->fmt) return e->alias_img;
    return e->img;
}
static VkImageView view_for(vktex_t *e, vkctx_t *c, VkFormat fmt, unsigned level, unsigned layer0, unsigned n) {
    for (int i = 0; i < e->nviews; i++)
        if (e->views[i].fmt == fmt && e->views[i].level == level && e->views[i].layer0 == layer0 && e->views[i].n == n) return e->views[i].v;
    // An attachment view needs only the attachment usage. Without saying so it inherits the image's whole usage, STORAGE included, which a view
    // in another format (sRGB) may not support.
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
        (e->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu, .image = view_image(e, fmt), .viewType = n > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = {e->aspect, level, 1, layer0, n}};
    VkImageView v;
    if (vkCreateImageView(c->dev, &vi, NULL, &v)) return VK_NULL_HANDLE;
    if (e->nviews == 16) { nvn_vk_defer_image(VK_NULL_HANDLE, e->views[0].v, VK_NULL_HANDLE); memmove(&e->views[0], &e->views[1], 15 * sizeof e->views[0]); e->nviews--; }
    e->views[e->nviews].fmt = fmt; e->views[e->nviews].level = level; e->views[e->nviews].layer0 = layer0; e->views[e->nviews].n = n; e->views[e->nviews++].v = v;
    return v;
}

// What a clear applies to: the level, the layers and the area (the scissor, clipped to the level), for the texture and view of a render target
static int clear_target(vktex_t *e, const view_t *v, int has_view, const int *rect, unsigned *level, unsigned *layer0, unsigned *n, VkFormat *fmt, VkRect2D *area) {
    *level = has_view && v->set_levels ? v->base_level : 0;
    *layer0 = has_view && v->set_layers ? v->min_layer : 0;
    *n = has_view && v->set_layers && v->nlayers ? v->nlayers : 1;
    *fmt = e->fmt;
    if (has_view && v->set_format) { VkFormat vf; if (nvn_fmt_to_vk(v->format, &vf, NULL) && vf != VK_FORMAT_UNDEFINED) *fmt = vf_fix(e, vf); }
    if (*level >= e->levels) return 0;
    if (e->is3d) { uint32_t dz = e->d.depth >> *level ? e->d.depth >> *level : 1; if (*layer0 + *n > dz) return 0; }   // the layers are depth slices (2D views of a 2D-array-compatible image)
    else if (*layer0 + *n > e->nsub) return 0;
    uint32_t lw, lh, lz; level_dims(e, *level, &lw, &lh, &lz);
    int x0 = 0, y0 = 0, x1 = (int)lw, y1 = (int)lh;
    if (rect) { x0 = rect[0] > 0 ? rect[0] : 0; y0 = rect[1] > 0 ? rect[1] : 0; x1 = rect[0] + rect[2] < x1 ? rect[0] + rect[2] : x1; y1 = rect[1] + rect[3] < y1 ? rect[1] + rect[3] : y1; }
    if (x1 <= x0 || y1 <= y0) { *n = 0; return 1; }                     // the scissor leaves nothing to clear
    *area = (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    return 1;
}
static void mark_gpu(vktex_t *e, unsigned level, unsigned layer0, unsigned n) {
    if (e->is3d) { __atomic_store_n(&SUB(e, level, 0), F_GPU, __ATOMIC_RELEASE); return; }   // a 3D level is one subresource
    for (unsigned ly = layer0; ly < layer0 + n && ly < e->nsub; ly++) __atomic_store_n(&SUB(e, level, ly), F_GPU, __ATOMIC_RELEASE);
}

void nvn_vk_clear_color(uint64_t serial, const view_t *v, int has_view, int mask, const float rgba[4], const int *rect) {
    static int partial, bad;
    vkctx_t *c = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd();
    if (!c || !cb) return;
    vktex_t *e = find(serial);
    if (!use(e, c, cb)) return;                                      // also uploads what the storage holds, which a scissored clear leaves partly in place
    if ((mask & 0xF) == 0) return;
    if ((mask & 0xF) != 0xF) { red_once(&partial, "a colour clear with a channel mask needs the draw path (not executed yet)", e->d.format); return; }
    unsigned level, layer0, n; VkFormat fmt; VkRect2D area;
    if (!clear_target(e, v, has_view, rect, &level, &layer0, &n, &fmt, &area) || !(e->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) { red_once(&bad, "the clear target is a 3D texture, a missing level or layer, or not a colour format", e->d.format); return; }
    if (!n) return;
    VkImageView iv = view_for(e, c, fmt, level, layer0, n);
    if (!iv) { red_once(&bad, "no attachment view for the clear", e->d.format); return; }
    VkClearValue cv = {0};
    if (fmt == VK_FORMAT_R8_UINT || fmt == VK_FORMAT_R8G8_UINT || fmt == VK_FORMAT_R8G8B8A8_UINT || fmt == VK_FORMAT_R16_UINT || fmt == VK_FORMAT_R16G16_UINT || fmt == VK_FORMAT_R16G16B16A16_UINT ||
        fmt == VK_FORMAT_R32_UINT || fmt == VK_FORMAT_R32G32_UINT || fmt == VK_FORMAT_R32G32B32A32_UINT || fmt == VK_FORMAT_A2B10G10R10_UINT_PACK32)
        for (int i = 0; i < 4; i++) cv.color.uint32[i] = rgba[i] <= 0 ? 0 : rgba[i] >= 4294967295.0f ? 0xFFFFFFFFu : (uint32_t)rgba[i];   // recorded as floats; exact up to 2^24
    else if (fmt == VK_FORMAT_R8_SINT || fmt == VK_FORMAT_R8G8_SINT || fmt == VK_FORMAT_R8G8B8A8_SINT || fmt == VK_FORMAT_R16_SINT || fmt == VK_FORMAT_R16G16_SINT ||
             fmt == VK_FORMAT_R16G16B16A16_SINT || fmt == VK_FORMAT_R32_SINT || fmt == VK_FORMAT_R32G32_SINT || fmt == VK_FORMAT_R32G32B32A32_SINT)
        for (int i = 0; i < 4; i++) cv.color.int32[i] = (int32_t)rgba[i];
    else for (int i = 0; i < 4; i++) cv.color.float32[i] = rgba[i];
    // A rendering pass of its own whose load operation is the clear: the render area is the scissor, so it limits what is cleared
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = iv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = cv};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = n, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri); vkCmdEndRendering(cb);
    nvn_vk_barrier_all(cb);
    mark_gpu(e, level, layer0, n);
}

void nvn_vk_clear_ds(uint64_t serial, const view_t *v, int has_view, float depth, int depth_mask, int stencil, int stencil_mask, const int *rect) {
    static int partial, bad;
    vkctx_t *c = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd();
    if (!c || !cb) return;
    vktex_t *e = find(serial);
    if (!use(e, c, cb)) return;
    unsigned sm = (unsigned)stencil_mask & 0xFF;
    if (sm && sm != 0xFF) { red_once(&partial, "a stencil clear with a partial mask needs the draw path (not executed yet)", e->d.format); sm = 0; }
    int do_d = depth_mask && (e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT), do_s = sm && (e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (!do_d && !do_s) return;
    unsigned level, layer0, n; VkFormat fmt; VkRect2D area;
    if (!clear_target(e, v, has_view, rect, &level, &layer0, &n, &fmt, &area) || !(e->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) { red_once(&bad, "the clear target is a 3D texture, a missing level or layer, or not a depth-stencil format", e->d.format); return; }
    if (!n) return;
    VkImageView iv = view_for(e, c, e->fmt, level, layer0, n);                 // a depth-stencil view keeps the image's own format
    if (!iv) { red_once(&bad, "no attachment view for the clear", e->d.format); return; }
    VkClearValue cv = {.depthStencil = {depth, (uint32_t)stencil}};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = iv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = do_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = cv};
    VkRenderingAttachmentInfo sa = da; sa.loadOp = do_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = n,
        .pDepthAttachment = (e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &da : NULL, .pStencilAttachment = (e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri); vkCmdEndRendering(cb);
    nvn_vk_barrier_all(cb);
    mark_gpu(e, level, layer0, n);
}

// A render target for the draw path: its view (the texture's own format, or the view's reinterpretation), the size of the level and the layers.
// Fills the image from its storage first when that is dirty, and marks what the view covers as GPU-written, since the draws will write it.
// The submission being recorded must not be inside a render pass (nvn_vk_cmd ends one).
// Compressed textures that the game writes through an integer view (BC3 as R32G32B32A32_UINT, etc.) need a second image with a compatible format and block-sized dimensions; it shares no memory with the compressed image in this version, so the draw executes but the sampled result may be stale until a copy is added.
static int bc_block_size(VkFormat f) {
    switch (f) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: return 8;
        case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return 16;
        default: return 0;
    }
}
static void ensure_alias(vktex_t *e, vkctx_t *c) {
    if (!e || e->has_alias) return;
    int bs = bc_block_size(e->fmt);
    if (!bs) return;
    VkFormat af = (bs == 8) ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT;
    uint32_t aw = (e->d.width + 3) / 4; if (!aw) aw = 1;
    uint32_t ah = (e->d.height + 3) / 4; if (!ah) ah = 1;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = af,
        .extent = {aw, ah, 1}, .mipLevels = 1, .arrayLayers = e->layers > 1 ? e->layers : 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(c->dev, &ii, NULL, &e->alias_img)) return;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(c->dev, e->alias_img, &mr);
    uint32_t mt = vkctx_memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { vkDestroyImage(c->dev, e->alias_img, NULL); e->alias_img = VK_NULL_HANDLE; return; }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    if (vkAllocateMemory(c->dev, &ai, NULL, &e->alias_vmem)) { vkDestroyImage(c->dev, e->alias_img, NULL); e->alias_img = VK_NULL_HANDLE; return; }
    if (vkBindImageMemory(c->dev, e->alias_img, e->alias_vmem, 0)) { vkFreeMemory(c->dev, e->alias_vmem, NULL); vkDestroyImage(c->dev, e->alias_img, NULL); e->alias_img = VK_NULL_HANDLE; e->alias_vmem = VK_NULL_HANDLE; return; }
    e->has_alias = 1;
}

int nvn_vk_tex_attach(uint64_t serial, const view_t *v, int has_view, nvn_attach *out) {
    static int bad;
    { static int lg = -1; if (lg < 0) lg = getenv("NX_SKIP_LOG") != NULL;     // with NX_SKIP_LOG: which serials fail, counted
      if (lg) { vktex_t *f = find(serial); static _Atomic unsigned nf; if (!f || !f->ok) { unsigned k = atomic_fetch_add(&nf, 1); uint64_t age = 0; int why = f ? 0 : gone_why(serial, &age);
          if (k < 40 || k % 500 == 0) fprintf(stderr, "[dm] [nvn] attach fail #%u serial %llu (%s, NVN format %#x)\n", k, (unsigned long long)serial,
              !f ? (why == GONE_SWEPT ? "swept from the table" : why == GONE_REINIT ? "left by a re-initialization" : why ? "finalized and freed" : "never registered") : "no image", f ? f->d.format : 0); } } }
    vkctx_t *c = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd();
    if (!c || !cb) return 0;
    vktex_t *e = find(serial); touch(e);
    if (!use(e, c, cb)) { static int none; if (!__atomic_exchange_n(&none, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] render target serial %llu has no Vulkan image (%s)\n", (unsigned long long)serial, e ? "its image could not be made" : "never registered"); return 0; }
    unsigned level, layer0, n; VkFormat fmt; VkRect2D area;
    if (!clear_target(e, v, has_view, NULL, &level, &layer0, &n, &fmt, &area) || !n) {
        if (!__atomic_exchange_n(&bad, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] render target serial %llu cannot be attached: NVN format %#x target %d, %ux%ux%u, %u levels, %u layers; view %d (levels %d base %u, layers %d min %u n %u)\n",
            (unsigned long long)serial, e->d.format, e->d.target, e->d.width, e->d.height, e->d.depth, e->levels, e->nsub, has_view, has_view && v->set_levels, has_view ? v->base_level : 0, has_view && v->set_layers, has_view ? v->min_layer : 0, has_view ? v->nlayers : 0);
        return 0; }
    VkFormat vf = (e->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? fmt : e->fmt;
    if (vf != e->fmt && bc_block_size(e->fmt) > 0) { ensure_alias(e, c); if (e->has_alias) e->alias_dirty = 1; }
    VkImageView iv = view_for(e, c, vf, level, layer0, n);
    if (!iv) return 0;
    mark_gpu(e, level, layer0, n);
    *out = (nvn_attach){iv, vf, area.extent.width, area.extent.height, n, e->aspect};
    return 1;
}

// ---- sampling
// NVN's texture target -> the Vulkan view type and the translator's SamplerType (nvn_shbind.aux: 1 1D, 3 2D, 4 3D, 5 cube, +256 for an array)
static int sample_type(unsigned target, VkImageViewType *vt, int *code, int *fixed_layers) {
    *fixed_layers = 1;
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: *vt = VK_IMAGE_VIEW_TYPE_1D; *code = 1; return 1;
    case NVN_TEXTURE_TARGET_2D: case NVN_TEXTURE_TARGET_RECTANGLE: *vt = VK_IMAGE_VIEW_TYPE_2D; *code = 3; return 1;
    case NVN_TEXTURE_TARGET_3D: *vt = VK_IMAGE_VIEW_TYPE_3D; *code = 4; return 1;
    case NVN_TEXTURE_TARGET_1D_ARRAY: *vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; *code = 1 | 256; *fixed_layers = 0; return 1;
    case NVN_TEXTURE_TARGET_2D_ARRAY: *vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; *code = 3 | 256; *fixed_layers = 0; return 1;
    case NVN_TEXTURE_TARGET_CUBEMAP: *vt = VK_IMAGE_VIEW_TYPE_CUBE; *code = 5; *fixed_layers = 6; return 1;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: *vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; *code = 5 | 256; *fixed_layers = 0; return 1;
    default: return 0;
    }
}
// A texture's swizzle (NVN: 0 zero 1 one 2 R 3 G 4 B 5 A) laid over the one its format implies
static VkComponentSwizzle swz_compose(unsigned user, const VkComponentSwizzle fmt[4]) {
    if (user == 0) return VK_COMPONENT_SWIZZLE_ZERO;
    if (user == 1) return VK_COMPONENT_SWIZZLE_ONE;
    unsigned c = user - 2 > 3 ? 0 : user - 2;
    return fmt[c] == VK_COMPONENT_SWIZZLE_IDENTITY ? (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + c) : fmt[c];
}

// A sampling view of a texture for the draw being prepared: the registered view's levels, layers, format and swizzle on the texture's own target. Fills the image from its storage first
// when that is dirty, which ends the render pass (a copy cannot happen inside one), so the caller opens the pass after calling this. *code is the SamplerType the view presents.
int nvn_vk_tex_sampled(uint64_t serial, const view_t *v, int has_view, int tex_set_swz, unsigned tex_swz, VkImageView *out, int *code) {
    static int bad;
    vkctx_t *c = nvn_vk_ctx();
    vktex_t *e = find(serial); touch(e);
    if (!c || !e || !e->ok) return 0;
    if (e->zero) recheck(e);                                        // uploaded as zeros: has the title filled the storage since?
    int dirty = !e->inited;
    for (size_t i = 0; i < (size_t)e->levels * e->nsub && !dirty; i++) dirty = (__atomic_load_n(&e->st[i], __ATOMIC_ACQUIRE) & F_DIRTY) != 0;
    if (dirty) { VkCommandBuffer cb = nvn_vk_cmd(); if (!cb || !use(e, c, cb)) return 0; }
    unsigned target = has_view && v->set_target ? v->target : e->d.target;
    VkImageViewType vt; int fixed;
    int want = *code;                                               // the SamplerType the shader declares (0: any)
    if (!sample_type(target, &vt, code, &fixed)) { red_once(&bad, "this texture target cannot be sampled yet", e->d.format); return 0; }
    if (want && want != *code && (want & 255) == (*code & 255) && ((want & 255) == 1 || (want & 255) == 3 || (want & 255) == 5)) {
        // the shader samples the array form of a plain texture or the reverse (a font's glyph sheets): Vulkan makes either view of the same image
        static const VkImageViewType plain[6] = {0, VK_IMAGE_VIEW_TYPE_1D, 0, VK_IMAGE_VIEW_TYPE_2D, 0, VK_IMAGE_VIEW_TYPE_CUBE};
        static const VkImageViewType arr[6] = {0, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
        vt = (want & 256) ? arr[want & 255] : plain[want & 255];
        fixed = (want & 256) ? 0 : ((want & 255) == 5 ? 6 : 1);
        *code = want;
    }
    else if ((want & 255) == 5 && (*code & 255) == 3 && !e->is3d && e->layers >= 6 && e->d.width == e->d.height) {
        // the shader samples a cube and the object is a 2D texture array of 6 layers (or a multiple): MK8D's reflection maps. The image is cube-compatible (mtl_tex_init)
        vt = (want & 256) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE; fixed = (want & 256) ? 0 : 6; *code = want;
    }
    if (want && want != *code) { static _Atomic int nm; if (atomic_fetch_add(&nm, 1) < 6)         // a sampler type the texture cannot give: what it is
        fprintf(stderr, "[dm] [nvn] sampler mismatch: texture serial %llu target %u %ux%ux%u, %u layers, %u levels, fmt %#x, view %d; the shader wants type %#x, the texture gives %#x\n",
                (unsigned long long)serial, target, e->d.width, e->d.height, e->d.depth, e->layers, e->levels, e->d.format, has_view, want, *code); }
    uint32_t l0 = has_view && v->set_levels ? v->base_level : 0, nl = has_view && v->set_levels && v->nlevels ? v->nlevels : e->levels - l0;
    uint32_t a0 = has_view && v->set_layers ? v->min_layer : 0, na = fixed ? (uint32_t)fixed : has_view && v->set_layers && v->nlayers ? v->nlayers : e->layers - a0;
    if (e->is3d) { a0 = 0; na = 1; }
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) na -= na % 6;
    if (l0 >= e->levels) return 0;
    if (l0 + nl > e->levels) nl = e->levels - l0;
    if (a0 + na > (e->is3d ? 1u : e->layers)) { red_once(&bad, "a texture view names layers the texture does not have", e->d.format); return 0; }
    VkFormat fmt = e->fmt; VkComponentSwizzle fs[4] = {0, 0, 0, 0};
    if (has_view && v->set_format) { VkFormat vf; VkComponentSwizzle s4[4] = {0, 0, 0, 0}; if (nvn_fmt_to_vk(v->format, &vf, s4) && vf != VK_FORMAT_UNDEFINED) { fmt = vf; memcpy(fs, s4, sizeof fs); } }
    else nvn_fmt_to_vk(e->d.format, &fmt, fs);
    fmt = vf_fix(e, fmt);
    unsigned user[4] = {2, 3, 4, 5}; int set = 0;
    if (has_view && v->set_swizzle) { for (int i = 0; i < 4; i++) user[i] = v->swz[i]; set = 1; }
    else if (tex_set_swz) { for (int i = 0; i < 4; i++) user[i] = (tex_swz >> (3 * i)) & 7; set = 1; }
    VkComponentMapping cm = {fs[0], fs[1], fs[2], fs[3]};
    if (set) cm = (VkComponentMapping){swz_compose(user[0], fs), swz_compose(user[1], fs), swz_compose(user[2], fs), swz_compose(user[3], fs)};
    uint32_t key = cm.r | cm.g << 4 | cm.b << 8 | cm.a << 12;
    for (int i = 0; i < e->nsvs; i++) {
        typeof(e->svs[0]) *s = &e->svs[i];
        if (s->fmt == fmt && s->l0 == l0 && s->nl == nl && s->a0 == a0 && s->na == na && s->vtype == (int)vt && s->swz == key) { *out = s->v; return 1; }
    }
    VkImageAspectFlags asp = (e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : e->aspect;
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu, .image = e->img, .viewType = vt, .format = fmt, .components = cm,
        .subresourceRange = {asp, l0, nl, a0, na}};
    VkImageView iv;
    if (vkCreateImageView(c->dev, &vi, NULL, &iv)) { red_once(&bad, "no sampling view for the texture", e->d.format); return 0; }
    if (e->nsvs == 8) { nvn_vk_defer_image(VK_NULL_HANDLE, e->svs[0].v, VK_NULL_HANDLE); memmove(&e->svs[0], &e->svs[1], 7 * sizeof e->svs[0]); e->nsvs--; }
    e->svs[e->nsvs++] = (typeof(e->svs[0])){fmt, l0, nl, a0, na, (int)vt, key, iv};
    *out = iv;
    return 1;
}
