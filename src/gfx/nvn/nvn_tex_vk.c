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
    uint32_t layers, levels;
    int ok;                                                    // an image exists and the format maps
    int astc;                                                  // an ASTC texture on a device without ASTC: the image is RGBA8 and upload decodes the blocks (tex/astc.c)
    int inited;                                                // moved out of UNDEFINED into GENERAL in a recorded submission
    uint32_t nsub;                                             // subresources per level: the layers, or 1 for a 3D texture
    uint8_t *st;                                               // levels * nsub bytes of F_DIRTY | F_GPU, accessed atomically
    struct { VkFormat fmt; unsigned level, layer0, n; VkImageView v; } views[16]; int nviews;   // attachment views made so far; touched by the submit thread only
    struct { VkFormat fmt; uint32_t l0, nl, a0, na; int vtype; uint32_t swz; VkImageView v; } svs[8]; int nsvs;   // sampling views, the same way
    uint64_t hash, hash_ms;                                    // the storage's hash and when it was taken (recheck below)
} vktex_t;
enum { F_DIRTY = 1, F_GPU = 2 };
#define SUB(e, lv, ly) ((e)->st[(size_t)(lv) * (e)->nsub + (ly)])

static vktex_t *g_bucket[NB];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void red_once(int *flag, const char *what, int fmt) {
    if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] texture: %s (NVN format %#x)\n", what, fmt);
}

static vktex_t *find(uint64_t serial) {
    pthread_mutex_lock(&g_mu);
    vktex_t *e = g_bucket[serial & (NB - 1)];
    while (e && e->serial != serial) e = e->next;
    pthread_mutex_unlock(&g_mu);
    return e;
}

// A view of an ASTC texture the device cannot hold as ASTC names the RGBA8 format the image has (sRGB or not as the requested ASTC format is)
static VkFormat vf_fix(const vktex_t *e, VkFormat f) {
    if (e->astc && f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return f;
}
static VkFormatFeatureFlags features(vkctx_t *c, VkFormat f) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(c->pd, f, &p); return p.optimalTilingFeatures; }

void mtl_tex_init(const tex_t *t) {
    vkctx_t *c = nvn_vk_ctx();
    if (!c || !t->serial) return;
    static int bad_fmt, bad_ms, bad_img;
    vktex_t *e = calloc(1, sizeof *e);
    e->serial = t->serial; e->d = t->d; e->L = t->L; e->mem = t->mem;
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
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
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
    pthread_mutex_unlock(&g_mu);
}

void mtl_tex_release(tex_t *t) {
    pthread_mutex_lock(&g_mu);
    for (vktex_t **p = &g_bucket[t->serial & (NB - 1)]; *p; p = &(*p)->next)
        if ((*p)->serial == t->serial) {
            vktex_t *e = *p; *p = e->next;
            pthread_mutex_unlock(&g_mu);
            for (int i = 0; i < e->nviews; i++) nvn_vk_defer_image(VK_NULL_HANDLE, e->views[i].v, VK_NULL_HANDLE);
            for (int i = 0; i < e->nsvs; i++) nvn_vk_defer_image(VK_NULL_HANDLE, e->svs[i].v, VK_NULL_HANDLE);
            if (e->img) nvn_vk_defer_image(e->img, VK_NULL_HANDLE, e->vmem);           // a submission in flight may still use it
            free(e->st); free(e);
            return;
        }
    pthread_mutex_unlock(&g_mu);
}

// The CPU wrote subresources: the storage is their truth again and the image must be filled from it
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
}

// De-swizzle the dirty subresources of the storage and copy them into the image
static void upload(vktex_t *e, vkctx_t *c, VkCommandBuffer cb) {
    static int skipped;
    if (!e->mem) return;
    if (e->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) { red_once(&skipped, "uploading a depth-stencil texture from storage is not supported yet", e->d.format); return; }
    if (e->d.samples > 1) return;
    size_t nreg = (size_t)e->levels * e->nsub, total = 0, n = 0;
    VkBufferImageCopy *rg = calloc(nreg ? nreg : 1, sizeof *rg);
    size_t *off = calloc(nreg ? nreg : 1, sizeof *off), *pitch = calloc(nreg ? nreg : 1, sizeof *pitch);
    unsigned *slv = calloc(nreg ? nreg : 1, sizeof *slv), *sly = calloc(nreg ? nreg : 1, sizeof *sly);
    for (unsigned lv = 0; lv < e->levels; lv++) {
        uint32_t w, h, z; level_dims(e, lv, &w, &h, &z);
        size_t wb = (size_t)((w + e->bw - 1) / e->bw) * e->bpb, hb = (h + e->bh - 1) / e->bh;
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
        size_t wb = (size_t)((w + e->bw - 1) / e->bw) * e->bpb, hb = (h + e->bh - 1) / e->bh, stride = e->d.stride ? e->d.stride : wb;
        for (size_t y = 0; y < hb; y++) memcpy((uint8_t *)st.map + off[0] + y * wb, e->mem + y * stride, wb);
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
            } else if (nvn_deswizzle(&e->d, slv[i], sly[i], e->mem, (uint8_t *)st.map + off[i], pitch[i])) {
                static _Atomic int nfail; if (atomic_fetch_add(&nfail, 1) < 8) fprintf(stderr, "[dm] [RED] [nvn] texture upload: de-swizzle of level %u layer %u failed (%ux%ux%u, NVN format %#x, target %d, flags %#x, stride %u): the level stays zero\n", slv[i], sly[i], e->d.width, e->d.height, e->d.depth, e->d.format, e->d.target, e->d.flags, e->d.stride);
                memset((uint8_t *)st.map + off[i], 0, pitch[i]);
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
static uint64_t storage_hash(const uint8_t *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull, w; size_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
static void recheck(vktex_t *e) {
    static int off = -1; if (off < 0) off = getenv("NX_TEX_NO_RECHECK") != NULL;
    if (off || !e->mem || !e->L.size || e->L.size > (64u << 20)) return;
    int cpu = 0; for (size_t i = 0; i < (size_t)e->levels * e->nsub && !cpu; i++) cpu = !(__atomic_load_n(&e->st[i], __ATOMIC_ACQUIRE) & F_GPU);
    if (!cpu) return;                                              // a render target: the GPU owns every subresource
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    if (e->hash_ms && ms - e->hash_ms < 100) return;
    e->hash_ms = ms;
    uint64_t h = storage_hash(e->mem, e->L.size);
    if (h == e->hash) return;
    int first = !e->hash; e->hash = h;
    if (first) return;                                             // the initial contents are uploaded by the F_DIRTY set at creation
    for (size_t i = 0; i < (size_t)e->levels * e->nsub; i++) {
        uint8_t f = __atomic_load_n(&e->st[i], __ATOMIC_ACQUIRE);
        if (!(f & F_GPU)) __atomic_store_n(&e->st[i], (uint8_t)(f | F_DIRTY), __ATOMIC_RELEASE);
    }
}
static int use(vktex_t *e, vkctx_t *c, VkCommandBuffer cb) {
    if (!e || !e->ok) return 0;
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
            vkbuf_t st;
            if (!cb || vkbuf_create(ctx, &st, wb * hb, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) { if (cb) nvn_vk_oneshot_end(cb); memset(dst + (size_t)z * img, 0, img); continue; }
            nvn_vk_barrier_all(cb);
            VkBufferImageCopy rg = {.bufferRowLength = (uint32_t)(wb / bpb) * bw, .bufferImageHeight = (uint32_t)hb * bh,
                .imageSubresource = {copy_aspect(e), level, layer, 1}, .imageOffset = {r->xoffset, r->yoffset, 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
            vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &rg);
            nvn_vk_barrier_all(cb);
            nvn_vk_oneshot_end(cb);
            for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, (uint8_t *)st.map + y * wb, wb);
            vkbuf_destroy(ctx, &st);
        } else read_storage(t, level, layer, 0, 1, z, r, dst, row, img, 0);
    }
    return;
gpu_slices: {
        vkctx_t *ctx = nvn_vk_ctx();
        mtl_finish();
        VkCommandBuffer cb = nvn_vk_oneshot_begin();
        vkbuf_t st;
        if (!cb || vkbuf_create(ctx, &st, wb * hb * nz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) { if (cb) nvn_vk_oneshot_end(cb); memset(dst, 0, img * nz); return; }
        nvn_vk_barrier_all(cb);
        VkBufferImageCopy rg = {.bufferRowLength = (uint32_t)(wb / bpb) * bw, .bufferImageHeight = (uint32_t)hb * bh, .imageSubresource = {copy_aspect(e), level, 0, 1},
            .imageOffset = {r->xoffset, r->yoffset, r->zoffset}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, (uint32_t)nz}};
        vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &rg);
        nvn_vk_barrier_all(cb);
        nvn_vk_oneshot_end(cb);
        for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, (uint8_t *)st.map + ((size_t)z * hb + y) * wb, wb);
        vkbuf_destroy(ctx, &st);
    }
}

// ---- render targets and clears
// A view of the image to attach: format (a view may reinterpret, an RGBA8_SRGB view of an RGBA8 texture), one mip level, `n` layers from `layer0`
static VkImageView view_for(vktex_t *e, vkctx_t *c, VkFormat fmt, unsigned level, unsigned layer0, unsigned n) {
    for (int i = 0; i < e->nviews; i++)
        if (e->views[i].fmt == fmt && e->views[i].level == level && e->views[i].layer0 == layer0 && e->views[i].n == n) return e->views[i].v;
    // An attachment view needs only the attachment usage. Without saying so it inherits the image's whole usage, STORAGE included, which a view
    // in another format (sRGB) may not support.
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
        (e->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu, .image = e->img, .viewType = n > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
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
    if (e->is3d || *level >= e->levels || *layer0 + *n > e->nsub) return 0;
    uint32_t lw, lh, lz; level_dims(e, *level, &lw, &lh, &lz);
    int x0 = 0, y0 = 0, x1 = (int)lw, y1 = (int)lh;
    if (rect) { x0 = rect[0] > 0 ? rect[0] : 0; y0 = rect[1] > 0 ? rect[1] : 0; x1 = rect[0] + rect[2] < x1 ? rect[0] + rect[2] : x1; y1 = rect[1] + rect[3] < y1 ? rect[1] + rect[3] : y1; }
    if (x1 <= x0 || y1 <= y0) { *n = 0; return 1; }                     // the scissor leaves nothing to clear
    *area = (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    return 1;
}
static void mark_gpu(vktex_t *e, unsigned level, unsigned layer0, unsigned n) {
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
int nvn_vk_tex_attach(uint64_t serial, const view_t *v, int has_view, nvn_attach *out) {
    static int bad;
    vkctx_t *c = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd();
    if (!c || !cb) return 0;
    vktex_t *e = find(serial);
    if (!use(e, c, cb)) { static int none; if (!__atomic_exchange_n(&none, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] render target serial %llu has no Vulkan image (%s)\n", (unsigned long long)serial, e ? "its image could not be made" : "never registered"); return 0; }
    unsigned level, layer0, n; VkFormat fmt; VkRect2D area;
    if (!clear_target(e, v, has_view, NULL, &level, &layer0, &n, &fmt, &area) || !n) { red_once(&bad, "the render target is a 3D texture or names a level or layer that does not exist", e->d.format); return 0; }
    VkFormat vf = (e->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? fmt : e->fmt;
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
    vktex_t *e = find(serial);
    if (!c || !e || !e->ok) return 0;
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
    uint32_t l0 = has_view && v->set_levels ? v->base_level : 0, nl = has_view && v->set_levels && v->nlevels ? v->nlevels : e->levels - l0;
    uint32_t a0 = has_view && v->set_layers ? v->min_layer : 0, na = fixed ? (uint32_t)fixed : has_view && v->set_layers && v->nlayers ? v->nlayers : e->layers - a0;
    if (e->is3d) { a0 = 0; na = 1; }
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
