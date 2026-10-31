// Texture and sampler descriptors on Vulkan: what the game registers into its pools (nvnTexturePoolRegisterTexture / nvnSamplerPoolRegisterSampler), by the host-table id
// hle_nvn.c numbers them with (every pool has its own base, so ids never collide), and the resolution of a bound handle (texture id | sampler id << 20) into a
// VkDescriptorImageInfo for a draw. The texture itself lives in nvn_tex_vk.c; a descriptor only remembers which one, and the view it asked for.
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_vk.h"
#include "nvn_vk_priv.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t serial; view_t v; uint8_t has_view, set_swz, used; uint16_t swz; } tdesc;
typedef struct { samp_par p; uint8_t used; VkSampler s; } sdesc;
static tdesc *g_tex; static int g_ntex;
static sdesc *g_samp; static int g_nsamp;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void *grow(void *p, int *n, int need, size_t sz) {
    if (need < *n) return p;
    int nn = *n ? *n : 256; while (nn <= need) nn *= 2;
    p = realloc(p, (size_t)nn * sz); memset((char *)p + (size_t)*n * sz, 0, (size_t)(nn - *n) * sz); *n = nn;
    return p;
}

void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id > (1 << 20)) return;
    if (!t) { static int once; if (!once++) fprintf(stderr, "[RED] nvn: RegisterTexture(id %d) with a NULL texture ignored\n", id); return; }
    pthread_mutex_lock(&g_mu);
    g_tex = grow(g_tex, &g_ntex, id, sizeof *g_tex);
    tdesc *d = &g_tex[id];
    *d = (tdesc){.serial = t->serial, .has_view = v != NULL, .set_swz = t->set_swz, .swz = t->swz, .used = 1};
    if (v) d->v = *v;
    pthread_mutex_unlock(&g_mu);
}

void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id > (1 << 20)) return;
    pthread_mutex_lock(&g_mu);
    g_samp = grow(g_samp, &g_nsamp, id, sizeof *g_samp);
    sdesc *d = &g_samp[id];
    if (!d->used || memcmp(&d->p, p, sizeof *p)) { d->p = *p; d->used = 1; d->s = VK_NULL_HANDLE; }   // a replaced sampler may still be in flight: the old object is left to the device teardown
    pthread_mutex_unlock(&g_mu);
}

// ---- NVN sampler state -> VkSamplerCreateInfo
static VkSamplerAddressMode wrap(unsigned w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;       // CLAMP (GL's legacy clamp) and CLAMP_TO_EDGE
    }
}
static VkSampler make_sampler(vkctx_t *c, const samp_par *p) {
    int mip = p->minf >= NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST;
    VkBorderColor bc = p->border[3] == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : (p->border[0] == 0 && p->border[1] == 0 && p->border[2] == 0) ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                     : (p->border[0] == 1 && p->border[1] == 1 && p->border[2] == 1) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    int aniso = c->enabled.samplerAnisotropy && p->aniso > 1.0f;
    float maxa = p->aniso > c->props.limits.maxSamplerAnisotropy ? c->props.limits.maxSamplerAnisotropy : p->aniso;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = p->magf == NVN_MAG_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
        .minFilter = (p->minf & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = p->minf >= NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap(p->ws), .addressModeV = wrap(p->wt), .addressModeW = wrap(p->wr),
        .mipLodBias = p->lod_bias, .anisotropyEnable = aniso, .maxAnisotropy = aniso ? maxa : 1.0f,
        .compareEnable = p->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE,
        .compareOp = p->cmp_func >= 1 && p->cmp_func <= 8 ? (VkCompareOp)(p->cmp_func - 1) : VK_COMPARE_OP_ALWAYS,
        .minLod = p->lod_min, .maxLod = mip ? p->lod_max : 0.0f, .borderColor = bc};     // NEAREST / LINEAR (no mipmap filter) read only the base level
    if (mip && si.maxLod < si.minLod) si.maxLod = 1000.0f;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(c->dev, &si, NULL, &s);
    return s;
}

// ---- what an empty slot reads: a 1x1 texture of zeros of the type the shader samples, with a nearest sampler (NVN: an unregistered descriptor reads 0)
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; int made; } dummy_t;
static dummy_t g_dummy[8];
static VkSampler g_dummy_samp;
static int dummy_index(int code) {
    int base = code & 0xFF, arr = (code >> 8) & 1, i = base == 1 ? 0 : base == 3 ? 1 : base == 4 ? 2 : base == 5 ? 3 : -1;
    return i < 0 || (base == 4 && arr) || (code & ~0x1FF) ? -1 : i * 2 + arr;
}
static int dummy_get(vkctx_t *c, int code, VkDescriptorImageInfo *out) {
    int di = dummy_index(code);
    if (di < 0) return 0;
    pthread_mutex_lock(&g_mu);
    dummy_t *d = &g_dummy[di];
    if (!d->made) {
        int base = code & 0xFF, arr = (code >> 8) & 1, cube = base == 5;
        uint32_t layers = cube ? 6 : 1;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
            .imageType = base == 1 ? VK_IMAGE_TYPE_1D : base == 4 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1},
            .mipLevels = 1, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkMemoryRequirements mr;
        if (vkCreateImage(c->dev, &ii, NULL, &d->img)) { pthread_mutex_unlock(&g_mu); return 0; }
        vkGetImageMemoryRequirements(c->dev, d->img, &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vkctx_memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkImageViewType vt = base == 1 ? (arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : base == 4 ? VK_IMAGE_VIEW_TYPE_3D
                           : cube ? (arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) : (arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = d->img, .viewType = vt, .format = ii.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
        VkCommandBuffer cb;
        if (ai.memoryTypeIndex == UINT32_MAX || nx_vk_alloc(c->dev, &ai, &d->mem) || vkBindImageMemory(c->dev, d->img, d->mem, 0) || vkCreateImageView(c->dev, &vi, NULL, &d->view) ||
            !(cb = vkctx_begin(c))) { pthread_mutex_unlock(&g_mu); return 0; }
        vk_barrier_image_all(cb, d->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkClearColorValue zero = {{0, 0, 0, 0}}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        vkCmdClearColorImage(cb, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rg);
        vk_barrier_image_all(cb, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vkctx_end(c, cb);
        d->made = 1;
    }
    if (!g_dummy_samp) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        vkCreateSampler(c->dev, &si, NULL, &g_dummy_samp);
    }
    *out = (VkDescriptorImageInfo){g_dummy_samp, d->view, VK_IMAGE_LAYOUT_GENERAL};
    pthread_mutex_unlock(&g_mu);
    return 1;
}

// What an empty slot reads for a shader that samples a depth texture with a comparison (2D): a 1x1 depth of 1.0 through a comparing sampler, so that the draw goes ahead and finds the
// point lit. (On the hardware an unregistered descriptor reads zeros; the title binds handle 0 there for a shadow it does not use, and a draw skipped is worse than one lit.)
static struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp; int made; } g_shadow;
static int shadow_dummy_get(vkctx_t *c, VkDescriptorImageInfo *out) {
    pthread_mutex_lock(&g_mu);
    if (!g_shadow.made) {
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkMemoryRequirements mr; VkCommandBuffer cb;
        if (vkCreateImage(c->dev, &ii, NULL, &g_shadow.img)) { pthread_mutex_unlock(&g_mu); return 0; }
        vkGetImageMemoryRequirements(c->dev, g_shadow.img, &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vkctx_memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_shadow.img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ii.format, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .compareEnable = VK_TRUE, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
        if (ai.memoryTypeIndex == UINT32_MAX || nx_vk_alloc(c->dev, &ai, &g_shadow.mem) || vkBindImageMemory(c->dev, g_shadow.img, g_shadow.mem, 0) ||
            vkCreateImageView(c->dev, &vi, NULL, &g_shadow.view) || vkCreateSampler(c->dev, &si, NULL, &g_shadow.smp) || !(cb = vkctx_begin(c))) { pthread_mutex_unlock(&g_mu); return 0; }
        vk_barrier_image_all(cb, g_shadow.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkClearDepthStencilValue one = {1.0f, 0}; VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, g_shadow.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &rg);
        vk_barrier_image_all(cb, g_shadow.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        vkctx_end(c, cb);
        g_shadow.made = 1;
    }
    *out = (VkDescriptorImageInfo){g_shadow.smp, g_shadow.view, VK_IMAGE_LAYOUT_GENERAL};
    pthread_mutex_unlock(&g_mu);
    return 1;
}

// A bound handle's texture and sampler as a descriptor; `want` is the SamplerType the shader declares for the binding (the shadow bit, 1024, is the sampler's business).
// 1 = filled; 0 = the texture or sampler cannot be used and there is no stand-in for this type; 3 = the texture is not of the type the shader samples.
// A texture id or sampler id that is not registered reads as an empty slot (zeros through a nearest sampler), as it does on the hardware.
uint64_t nvn_vk_desc_serial(uint32_t handle) {          // debugging: the texture serial a combined handle names (0 if the descriptor was never registered)
    uint32_t tid = handle & 0xfffff; pthread_mutex_lock(&g_mu); uint64_t r = (int)tid < g_ntex && g_tex[tid].used ? g_tex[tid].serial : 0; pthread_mutex_unlock(&g_mu); return r;
}
int nvn_vk_desc_sampled(uint32_t handle, int want, VkDescriptorImageInfo *out) {
    vkctx_t *c = nvn_vk_ctx();
    uint32_t tid = handle & 0xfffff, sid = handle >> 20;
    if (!c) return 0;
    int shadow = (want & 1024) != 0; want &= ~1024;
    pthread_mutex_lock(&g_mu);
    tdesc td = (int)tid < g_ntex ? g_tex[tid] : (tdesc){0};
    sdesc *sd = (int)sid < g_nsamp && g_samp[sid].used ? &g_samp[sid] : NULL;
    if (sd && !sd->s) sd->s = make_sampler(c, &sd->p);
    VkSampler smp = sd ? sd->s : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
    if (!td.used && !shadow) { static _Atomic unsigned nun; unsigned k = atomic_fetch_add(&nun, 1) + 1; if (k <= 8 || !(k & (k - 1))) fprintf(stderr, "[dm] [nvn] stand-in #%u: texture id %u is not registered (the shader samples type %#x, sampler %u)\n", k, tid, want, sid); }
    if (!td.used) { if (!shadow) { int r = dummy_get(c, want, out); if (!r) { static _Atomic int nn; if (atomic_fetch_add(&nn, 1) < 6) fprintf(stderr, "[dm] [nvn] sampled: texture id %u is not registered and there is no stand-in for sampler type %#x\n", tid, want); } return r; } return want == 3 ? shadow_dummy_get(c, out) : 0; }
    VkImageView iv; int code = want;
    if (!nvn_vk_tex_sampled(td.serial, &td.v, td.has_view, td.set_swz, td.swz, &iv, &code)) {
        if (shadow) return 0;
        { static _Atomic unsigned nnv; unsigned k = atomic_fetch_add(&nnv, 1) + 1; if (k <= 8 || !(k & (k - 1))) fprintf(stderr, "[dm] [nvn] stand-in #%u: texture id %u (serial %llu) has no view the shader can sample (type %#x)\n", k, tid, (unsigned long long)td.serial, want); }
        int r = dummy_get(c, want, out);
        if (!r) { static _Atomic int nn; if (atomic_fetch_add(&nn, 1) < 6) fprintf(stderr, "[dm] [nvn] sampled: texture id %u (serial %llu) gave no view and there is no stand-in for sampler type %#x\n", tid, (unsigned long long)td.serial, want); }
        return r; }
    if (code != want && (want & 255) == 5) {      // the shader declares a cube and the texture cannot be one (a 2D texture, or a 2D array of fewer than six layers: a placeholder for an environment map): it reads the zero cube, as nxnative's tex_fit does
        static int seen_cube; if (!__atomic_exchange_n(&seen_cube, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [nvn] cube binding fallback: texture id %u (sampler type %#x) bound as the zero cube for a shader that declares %#x\n", tid, code, want);
        return dummy_get(c, want, out);
    }
    if (code != want) {
        static int seen; if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] texture id %u is sampler type %#x, the shader samples %#x\n", tid, code, want);
        return 3;
    }
    if (!smp) { VkDescriptorImageInfo tmp; if (!dummy_get(c, 3, &tmp)) return 0; smp = tmp.sampler; }
    *out = (VkDescriptorImageInfo){smp, iv, VK_IMAGE_LAYOUT_GENERAL};
    return 1;
}
