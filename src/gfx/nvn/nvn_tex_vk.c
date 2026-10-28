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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int inited;                                                // moved out of UNDEFINED into GENERAL in a recorded submission
    uint32_t nsub;                                             // subresources per level: the layers, or 1 for a 3D texture
    uint8_t *st;                                               // levels * nsub bytes of F_DIRTY | F_GPU, accessed atomically
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
    int kind = nvn_fmt_to_vk(t->d.format, &e->fmt, NULL);
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) goto keep;                                           // buffer textures are views of pool memory, not images
    if (t->d.samples > 1) { red_once(&bad_ms, "multisampled textures are not supported yet", t->d.format); goto keep; }
    if (!kind || !nvn_fmt_vk_supported(c->pd, e->fmt)) { red_once(&bad_fmt, kind ? "the format has no Vulkan support on this device (ASTC and similar need a decoder)" : "the format has no Vulkan counterpart (3-component formats need expanding)", t->d.format); goto keep; }
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
            off[n] = total; pitch[n] = wb; slv[n] = lv; sly[n] = ly; total += wb * hb * z;
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
        for (size_t i = 0; i < n; i++)
            if (nvn_deswizzle(&e->d, slv[i], sly[i], e->mem, (uint8_t *)st.map + off[i], pitch[i])) memset((uint8_t *)st.map + off[i], 0, pitch[i]);
    }
    to_general(e, cb);
    vkCmdCopyBufferToImage(cb, st.buf, e->img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)n, rg);
    nvn_vk_barrier_all(cb);
    nvn_vk_keep(&st);
done:
    free(rg); free(off); free(pitch); free(slv); free(sly);
}

// Make the image current for a GPU use in the submission being recorded; returns 0 when the texture has no image
static int use(vktex_t *e, vkctx_t *c, VkCommandBuffer cb) {
    if (!e || !e->ok) return 0;
    to_general(e, cb);
    upload(e, c, cb);                                              // only what is dirty
    return 1;
}

// ---- buffer <-> texture copies
// One region per copy: the level, the first layer and the buffer layout come from the record and the stride state
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
    size_t wb = (size_t)((c->r.width + e->bw - 1) / e->bw) * e->bpb, hb = (c->r.height + e->bh - 1) / e->bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    if (row % e->bpb || img % row || row < wb) { fprintf(stderr, "[dm] [RED] [nvn] %s: row stride %llu / image stride %llu do not fit %d-byte blocks\n", what, (unsigned long long)row, (unsigned long long)img, e->bpb); return 0; }
    VkDeviceSize boff;
    if (!nvn_vk_pool_buffer(c->src, img * (uint64_t)nz, buf, &boff)) { fprintf(stderr, "[dm] [RED] [nvn] %s: the buffer address %#llx is in no memory pool\n", what, (unsigned long long)c->src); return 0; }
    if (boff % e->bpb || boff % 4) { fprintf(stderr, "[dm] [RED] [nvn] %s: buffer offset %llu is not aligned to the texel block\n", what, (unsigned long long)boff); return 0; }
    *out = (VkBufferImageCopy){.bufferOffset = boff, .bufferRowLength = (uint32_t)(row / e->bpb) * e->bw, .bufferImageHeight = (uint32_t)(img / row) * e->bh,
        .imageSubresource = {e->aspect, level, e->is3d ? 0 : layer0 + c->r.zoffset, e->is3d ? 1u : (uint32_t)nz},
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
    uint8_t *lin = malloc(lp * lr * lz);
    if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { fprintf(stderr, "[dm] [RED] [nvn] ReadTexels: de-swizzling failed (format %#x target %d)\n", t->d.format, t->d.target); free(lin); return; }
    for (int z = 0; z < nzz; z++) {
        const uint8_t *src = lin + (is3d ? (size_t)(z0 + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)(zbase + z) * img + y * row, src + (y0 + y) * lp + x0, wb);
    }
    free(lin);
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
                .imageSubresource = {e->aspect, level, layer, 1}, .imageOffset = {r->xoffset, r->yoffset, 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
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
        VkBufferImageCopy rg = {.bufferRowLength = (uint32_t)(wb / bpb) * bw, .bufferImageHeight = (uint32_t)hb * bh, .imageSubresource = {e->aspect, level, 0, 1},
            .imageOffset = {r->xoffset, r->yoffset, r->zoffset}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, (uint32_t)nz}};
        vkCmdCopyImageToBuffer(cb, e->img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &rg);
        nvn_vk_barrier_all(cb);
        nvn_vk_oneshot_end(cb);
        for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, (uint8_t *)st.map + ((size_t)z * hb + y) * wb, wb);
        vkbuf_destroy(ctx, &st);
    }
}
