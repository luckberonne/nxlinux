// Texture and sampler descriptors on Vulkan: what the game registers into its pools (nvnTexturePoolRegisterTexture / nvnSamplerPoolRegisterSampler), by the host-table id
// hle_nvn.c numbers them with (every pool has its own base, so ids never collide), and the resolution of a bound handle (texture id | sampler id << 20) into a
// VkDescriptorImageInfo for a draw. The texture itself lives in nvn_tex_vk.c; a descriptor only remembers which one, and the view it asked for.
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_vk.h"
#include "nvn_vk_priv.h"
#include <pthread.h>
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

// A bound handle's texture and sampler as a descriptor; `want` is the SamplerType the shader declares for the binding.
// 1 = filled; 0 = the texture id is not registered or has no image; 2 = the sampler id is not registered; 3 = the texture is not of the type the shader samples
int nvn_vk_desc_sampled(uint32_t handle, int want, VkDescriptorImageInfo *out) {
    vkctx_t *c = nvn_vk_ctx();
    uint32_t tid = handle & 0xfffff, sid = handle >> 20;
    if (!c) return 0;
    pthread_mutex_lock(&g_mu);
    tdesc td = (int)tid < g_ntex ? g_tex[tid] : (tdesc){0};
    sdesc *sd = (int)sid < g_nsamp && g_samp[sid].used ? &g_samp[sid] : NULL;
    if (sd && !sd->s) sd->s = make_sampler(c, &sd->p);
    VkSampler smp = sd ? sd->s : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
    if (!td.used) return 0;
    if (!smp) return 2;
    VkImageView iv; int code;
    if (!nvn_vk_tex_sampled(td.serial, &td.v, td.has_view, td.set_swz, td.swz, &iv, &code)) return 0;
    if (code != want) return 3;
    *out = (VkDescriptorImageInfo){smp, iv, VK_IMAGE_LAYOUT_GENERAL};
    return 1;
}
