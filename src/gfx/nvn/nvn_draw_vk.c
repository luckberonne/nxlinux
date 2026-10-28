// Draws on Vulkan. The replay hands state records (nvn_draw_state_op) and draws; a draw resolves the program's shaders from the registry, builds a pipeline
// from the whole state (cached), opens a render pass over the current targets if none is open, binds the vertex buffers and pushes the uniform buffers.
//
// Conventions of NVN, which is GL-like, mapped onto Vulkan:
//  - depth: NVN's default clip volume is -w..w; VK_EXT_depth_clip_control (negativeOneToOne) makes Vulkan's the same, so no shader changes.
//  - window origin: the default is the lower left. A render target is stored row 0 first either way, and NDC y = -1 lands on row 0 with a positive-height
//    Vulkan viewport, as in GL, so the lower-left viewport is not flipped; Vulkan names winding by how it looks with y down, which reverses the sense of
//    the same coordinates, so the front face is inverted. An upper-left origin flips the viewport instead and keeps the winding.
// A render pass stays open from the first draw until something else needs the command buffer (nvn_vk_cmd ends it) or the render targets change.
#include "nvn_draw.h"
#include "nvn_vk.h"
#include "nvn_vk_priv.h"
#include "nvn_shader_vk.h"
#include "../tex/nvn_fmt_vk.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct nvn_shader nvn_shader;
const nvn_shader *nvn_shader_find(const uint8_t key[8]);
int nvn_shader_stage(const nvn_shader *s);
const nvn_shbind *nvn_shader_binds(const nvn_shader *s, int *n);
VkShaderModule nvn_shader_module(nvn_shader *s);

nvn_rt_t g_rt;
struct nvn_scissor_state g_sc;
static _Atomic uint64_t g_ndraw, g_nskip, g_npipe;
uint64_t nvn_draw_count(void) { return atomic_load(&g_ndraw); }
uint64_t nvn_draw_skipped(void) { return atomic_load(&g_nskip); }
uint64_t nvn_pipeline_count(void) { return atomic_load(&g_npipe); }

// ---- state
static struct {
    int init;
    const prog_t *prog; int nkeys; uint8_t keys[6][8];
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    uint32_t attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16];
    float vp[16][6]; int has_vp; float dr[2]; int has_dr; p_poly_offset po; float blend_color[4];
    int st_vmask[2], st_wmask[2], st_ref[2];
    int depth_clamp, rast_discard; p_prim_restart restart;
} S;
static _Atomic int g_origin_ll = 1, g_depth_zero;   // NVN's defaults: lower-left origin, -w..w depth
static int g_queue_ll;

void mtl_set_origin_lower_left(int on) { atomic_store(&g_origin_ll, on != 0); }
void mtl_set_queue_origin_lower_left(int on) { g_queue_ll = on != 0; }
void mtl_set_depth_zero(int on) { atomic_store(&g_depth_zero, on != 0); }

static void state_init(void) {
    if (S.init) return;
    S.init = 1;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    S.chmask.mask = 0xffffffffu;
    S.ms.enable = 1;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds.df = NVN_DEPTH_FUNC_LESS; S.ds.dw = 1; S.ds.dt = 1;     // overwritten at once by the game's bound state; see nvnDepthStencilStateSetDefaults
    S.ds.sf_f = S.ds.sf_b = NVN_STENCIL_FUNC_ALWAYS;
    S.ds.fail_f = S.ds.zfail_f = S.ds.zpass_f = S.ds.fail_b = S.ds.zfail_b = S.ds.zpass_b = NVN_STENCIL_OP_KEEP;
    for (int f = 0; f < 2; f++) S.st_vmask[f] = S.st_wmask[f] = 0xff;
}

int nvn_draw_state_op(uint32_t op, const void *a) {
    state_init();
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = a;
        if (b->stages & 0x1f) { S.prog = b->prog; S.nkeys = b->prog ? nvn_prog_keys(b->prog, S.keys) : 0; }
        return 1; }
    case OP_BIND_STATE: {
        const p_bind_state *b = a; blend_st bs;
        switch (b->kind) {
        case 0: memcpy(&bs, b->bytes, sizeof bs); if (bs.target < 8) S.blend[bs.target] = bs; break;
        case 1: memcpy(&S.color, b->bytes, sizeof S.color); break;
        case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
        case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break;
        case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break;
        case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
        }
        return 1; }
    case OP_BIND_VATTR: { const p_bind_vattr *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.attr[b->first + i] = b->st[i]; S.nattr = b->first + b->n > 16 ? 16 : b->first + b->n; return 1; }
    case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.stream[b->first + i] = b->st[i]; S.nstream = b->first + b->n > 16 ? 16 : b->first + b->n; return 1; }
    case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); for (int i = 0; i < b->n && b->first + i < 16; i++) S.vbuf[b->first + i] = b->r[i]; return 1; }
    case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) S.ubo[b->stage][b->index] = *b; return 1; }
    case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S.vp[b->first + i], b->v[i], sizeof S.vp[0]); S.has_vp = 1; return 1; }
    case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; S.has_dr = 1; return 1; }
    case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; return 1;
    case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; return 1;
    case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); return 1;
    case OP_STENCIL: {
        const p_stencil *b = a;
        for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; }
        return 1; }
    case OP_PRIM_RESTART: S.restart = *(const p_prim_restart *)a; return 1;
    case OP_RAST_DISCARD: S.rast_discard = ((const p_rast_discard *)a)->on; return 1;
    default: return 0;
    }
}

// ---- NVN enums -> Vulkan
static VkBlendFactor bfactor(unsigned f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO; case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop(unsigned e) {
    switch (e) { case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT; case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
                 case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN; case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
static VkCompareOp cmp(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }   // NVN's NEVER..ALWAYS (1..8) are Vulkan's 0..7 in the same order
static VkStencilOp sop(unsigned o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }    // KEEP, ZERO, REPLACE, INCR, DECR, INVERT, INCR_WRAP, DECR_WRAP: the same order
static int topology(unsigned prim, VkPrimitiveTopology *t, int geom) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return 1;
    case NVN_DRAW_PRIMITIVE_LINES: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return 1;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return 1;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return 1;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return 1;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return 1;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; return geom;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; return geom;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; return geom;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; return geom;
    default: return 0;      // line loops, quads, polygons and patches need an index list built for them
    }
}
static int is_strip(VkPrimitiveTopology t) {
    return t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
           t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
}

// ---- pipelines. The key holds everything a pipeline is built from, already in Vulkan terms, with every unused byte zero so that memcmp compares it
typedef struct {
    const nvn_shader *vs, *fs;
    uint8_t ncolor; VkFormat cfmt[8]; VkFormat dfmt, sfmt;
    uint8_t topo, restart, cull, front_ccw, polymode, depth_clamp, bias_en, discard, neg1;
    uint8_t dt, dw, dfunc, st_en; struct { uint8_t func, fail, zfail, zpass; } sf[2];
    struct { uint8_t en, srgb, drgb, sa, da, erg, ea, mask; } cb[8];
    uint8_t nattr, nbind; struct { VkFormat fmt; uint32_t off; uint8_t binding; } at[16]; struct { uint32_t stride; uint8_t inst; } bd[16];
} pkey;

typedef struct pipe_entry { pkey key; VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSetLayout dsl; struct pipe_entry *next; } pipe_entry;
static pipe_entry *g_pipes;
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;

static VkPipelineLayout make_layout(vkctx_t *c, const pkey *k, VkDescriptorSetLayout *dsl_out) {
    VkDescriptorSetLayoutBinding lb[32]; int nb = 0;
    const nvn_shader *sh[2] = {k->vs, k->fs};
    for (int si = 0; si < 2; si++) {
        int n; const nvn_shbind *b = nvn_shader_binds(sh[si], &n);
        for (int i = 0; i < n; i++) {
            VkShaderStageFlags st = si ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            int j = 0; for (; j < nb; j++) if (lb[j].binding == b[i].binding) break;
            if (j == nb) { if (nb == 32) continue; lb[nb++] = (VkDescriptorSetLayoutBinding){b[i].binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL}; }
            lb[j].stageFlags |= st;
        }
    }
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = nb, .pBindings = lb};
    VkDescriptorSetLayout dsl; VkPipelineLayout pl;
    if (vkCreateDescriptorSetLayout(c->dev, &di, NULL, &dsl)) return VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
    if (vkCreatePipelineLayout(c->dev, &li, NULL, &pl)) { vkDestroyDescriptorSetLayout(c->dev, dsl, NULL); return VK_NULL_HANDLE; }
    *dsl_out = dsl;
    return pl;
}

static pipe_entry *get_pipeline(vkctx_t *c, const pkey *k) {
    pthread_mutex_lock(&g_pmu);
    for (pipe_entry *e = g_pipes; e; e = e->next) if (!memcmp(&e->key, k, sizeof *k)) { pthread_mutex_unlock(&g_pmu); return e; }
    pthread_mutex_unlock(&g_pmu);
    pipe_entry *e = calloc(1, sizeof *e); e->key = *k;
    e->layout = make_layout(c, k, &e->dsl);
    if (!e->layout) { free(e); return NULL; }
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = nvn_shader_module((nvn_shader *)k->vs), .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = nvn_shader_module((nvn_shader *)k->fs), .pName = "main"}};
    VkVertexInputBindingDescription vb[16]; VkVertexInputAttributeDescription va[16];
    for (int i = 0; i < k->nbind; i++) vb[i] = (VkVertexInputBindingDescription){(uint32_t)i, k->bd[i].stride, k->bd[i].inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    for (int i = 0; i < k->nattr; i++) va[i] = (VkVertexInputAttributeDescription){(uint32_t)i, k->at[i].binding, k->at[i].fmt, k->at[i].off};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = k->nbind, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = k->nattr, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = (VkPrimitiveTopology)k->topo, .primitiveRestartEnable = k->restart};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, k->neg1};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, c->has_depth_clip_control ? (const void *)&dcc : NULL, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = k->depth_clamp, .rasterizerDiscardEnable = k->discard,
        .polygonMode = (VkPolygonMode)k->polymode, .cullMode = k->cull, .frontFace = k->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE, .depthBiasEnable = k->bias_en, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkStencilOpState so[2];
    for (int f = 0; f < 2; f++) so[f] = (VkStencilOpState){(VkStencilOp)k->sf[f].fail, (VkStencilOp)k->sf[f].zpass, (VkStencilOp)k->sf[f].zfail, (VkCompareOp)k->sf[f].func, 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = k->dt, .depthWriteEnable = k->dw,
        .depthCompareOp = (VkCompareOp)k->dfunc, .stencilTestEnable = k->st_en, .front = so[0], .back = so[1]};
    VkPipelineColorBlendAttachmentState ba[8];
    for (int i = 0; i < k->ncolor; i++)
        ba[i] = (VkPipelineColorBlendAttachmentState){k->cb[i].en, (VkBlendFactor)k->cb[i].srgb, (VkBlendFactor)k->cb[i].drgb, (VkBlendOp)k->cb[i].erg,
                                                      (VkBlendFactor)k->cb[i].sa, (VkBlendFactor)k->cb[i].da, (VkBlendOp)k->cb[i].ea, k->cb[i].mask};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor, .pAttachments = ba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = k->cfmt,
                                        .depthAttachmentFormat = k->dfmt, .stencilAttachmentFormat = k->sfmt};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = e->layout};
    if (vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, &e->pipe)) { fprintf(stderr, "[dm] [RED] [nvn] pipeline creation failed\n"); e->pipe = VK_NULL_HANDLE; }
    atomic_fetch_add(&g_npipe, 1);
    pthread_mutex_lock(&g_pmu); e->next = g_pipes; g_pipes = e; pthread_mutex_unlock(&g_pmu);
    return e;
}

// ---- the render pass
static struct { int active; nvn_rt_t rt; nvn_attach col[8], dep; int have_dep; uint32_t w, h, layers; VkPipeline bound; } P;

void nvn_draw_pass_end(void) {
    VkCommandBuffer cb = nvn_vk_cmd_raw();
    if (!P.active || !cb) { P.active = 0; return; }
    vkCmdEndRendering(cb);
    P.active = 0;
    nvn_vk_barrier_all(cb);                 // what the draws wrote is visible to whatever comes next
}
void nvn_draw_new_submission(void) { P.active = 0; P.bound = VK_NULL_HANDLE; }

static int rt_same(const nvn_rt_t *a, const nvn_rt_t *b) { return !memcmp(a, b, sizeof *a); }

static int pass_begin(vkctx_t *c, VkCommandBuffer cb) {
    (void)c;
    nvn_attach *col = P.col, *dep = &P.dep; int have_dep = 0;
    VkRenderingAttachmentInfo ca[8], da, sa;
    uint32_t w = 0, h = 0, layers = 1;
    nvn_draw_pass_end();                                          // the targets may differ from the open pass's
    for (int i = 0; i < g_rt.n; i++) {
        memset(&col[i], 0, sizeof col[i]);
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!g_rt.cser[i]) continue;
        if (!nvn_vk_tex_attach(g_rt.cser[i], &g_rt.cview[i], g_rt.has_cview[i], &col[i])) return 0;
        ca[i].imageView = col[i].view; ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!w || col[i].w < w) w = col[i].w; if (!h || col[i].h < h) h = col[i].h; layers = col[i].layers;
    }
    if (g_rt.dser) {
        if (!nvn_vk_tex_attach(g_rt.dser, &g_rt.dview, g_rt.has_dview, dep)) return 0;
        have_dep = 1;
        da = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dep->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        sa = da;
        if (!w || dep->w < w) w = dep->w; if (!h || dep->h < h) h = dep->h; layers = dep->layers;
    }
    if (!w || !h) return 0;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = layers, .colorAttachmentCount = (uint32_t)g_rt.n, .pColorAttachments = ca,
        .pDepthAttachment = have_dep && (dep->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &da : NULL, .pStencilAttachment = have_dep && (dep->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    P.active = 1; P.have_dep = have_dep; P.rt = g_rt; P.w = w; P.h = h; P.layers = layers;
    return 1;
}

// ---- a draw
static VkBuffer zero_buffer(vkctx_t *c) {                         // what an unbound uniform slot reads
    static vkbuf_t z; static int made;
    if (!made) { made = 1; if (!vkbuf_create(c, &z, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1)) memset(z.map, 0, 65536); }
    return z.buf;
}
static void skip(const char *why, int *flag) {
    atomic_fetch_add(&g_nskip, 1);
    if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] draw skipped: %s\n", why);
}

// Everything shared by the plain and the indexed draw: the pipeline, the pass, the dynamic state, the vertex buffers and the uniforms. 0 when the draw cannot be made.
static int prepare(unsigned prim, pipe_entry **pe_out) {
    static int f_noprog, f_noshader, f_prim, f_vbuf, f_vattr, f_nort, f_pipe, f_fmt, f_off;
    state_init();
    vkctx_t *c = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd_raw();
    if (!c || !cb) return 0;
    if (!c->has_push_descriptor) { skip("the device lacks VK_KHR_push_descriptor", &f_off); return 0; }
    if (!S.prog || !S.nkeys) { skip("no program is bound", &f_noprog); return 0; }
    const nvn_shader *vs = NULL, *fs = NULL;
    for (int i = 0; i < S.nkeys; i++) {
        const nvn_shader *s = nvn_shader_find(S.keys[i]);
        if (!s) continue;
        int st = nvn_shader_stage(s);
        if (st == NVN_STAGE_VERTEX) vs = s; else if (st == NVN_STAGE_FRAGMENT) fs = s;
        else { skip("geometry, tessellation and compute stages are not supported in a draw yet", &f_noshader); return 0; }
    }
    if (!vs || !fs) { skip("the program's shaders are not in the registry (the translated shader library is not loaded)", &f_noshader); return 0; }
    VkPrimitiveTopology topo;
    if (!topology(prim, &topo, c->enabled.geometryShader)) { skip("this primitive type needs an index list built for it", &f_prim); return 0; }
    if (!g_rt.n && !g_rt.dser) { skip("no render target is set", &f_nort); return 0; }

    pkey k; memset(&k, 0, sizeof k);
    k.vs = vs; k.fs = fs; k.topo = (uint8_t)topo; k.restart = S.restart.on && is_strip(topo);
    // The pass first: it decides the attachment formats the pipeline is built for
    if (!P.active || !rt_same(&P.rt, &g_rt)) { if (!pass_begin(c, cb)) { skip("a render target could not be attached", &f_pipe); return 0; } }
    k.ncolor = (uint8_t)g_rt.n;
    for (int i = 0; i < g_rt.n; i++) k.cfmt[i] = g_rt.cser[i] ? P.col[i].fmt : VK_FORMAT_UNDEFINED;
    if (P.have_dep) {
        k.dfmt = (P.dep.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? P.dep.fmt : VK_FORMAT_UNDEFINED;
        k.sfmt = (P.dep.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? P.dep.fmt : VK_FORMAT_UNDEFINED;
    }
    int ll = atomic_load(&g_origin_ll);
    k.cull = S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : S.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : 0;
    k.front_ccw = (S.poly.front == NVN_FRONT_FACE_CCW) ^ (ll != 0);       // see the conventions at the top
    k.polymode = S.poly.mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : S.poly.mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    if (k.polymode != VK_POLYGON_MODE_FILL && !c->enabled.fillModeNonSolid) k.polymode = VK_POLYGON_MODE_FILL;
    k.depth_clamp = S.depth_clamp && c->enabled.depthClamp;
    k.bias_en = (S.poly.offset_en >> (S.poly.mode == NVN_POLYGON_MODE_POINT ? 0 : S.poly.mode == NVN_POLYGON_MODE_LINE ? 1 : 2)) & 1;
    k.discard = S.rast_discard != 0;
    k.neg1 = !atomic_load(&g_depth_zero);
    int has_depth = k.dfmt != VK_FORMAT_UNDEFINED, has_stencil = k.sfmt != VK_FORMAT_UNDEFINED;
    k.dt = S.ds.dt && has_depth; k.dw = S.ds.dw && has_depth; k.dfunc = (uint8_t)cmp(S.ds.df); k.st_en = S.ds.st && has_stencil;
    k.sf[0].func = (uint8_t)cmp(S.ds.sf_f); k.sf[0].fail = (uint8_t)sop(S.ds.fail_f); k.sf[0].zfail = (uint8_t)sop(S.ds.zfail_f); k.sf[0].zpass = (uint8_t)sop(S.ds.zpass_f);
    k.sf[1].func = (uint8_t)cmp(S.ds.sf_b); k.sf[1].fail = (uint8_t)sop(S.ds.fail_b); k.sf[1].zfail = (uint8_t)sop(S.ds.zfail_b); k.sf[1].zpass = (uint8_t)sop(S.ds.zpass_b);
    for (int i = 0; i < k.ncolor; i++) {
        int t = c->enabled.independentBlend ? i : 0; const blend_st *b = &S.blend[t];
        k.cb[i].en = (S.color.blend_en >> t) & 1;
        k.cb[i].srgb = (uint8_t)bfactor(b->src_rgb); k.cb[i].drgb = (uint8_t)bfactor(b->dst_rgb); k.cb[i].sa = (uint8_t)bfactor(b->src_a); k.cb[i].da = (uint8_t)bfactor(b->dst_a);
        k.cb[i].erg = (uint8_t)bop(b->eq_rgb); k.cb[i].ea = (uint8_t)bop(b->eq_a);
        k.cb[i].mask = (S.chmask.mask >> (4 * i)) & 0xF;
    }
    // Vertex input: attribute i is location i; its stream is the binding
    k.nbind = (uint8_t)S.nstream; k.nattr = (uint8_t)S.nattr;
    for (int i = 0; i < S.nstream; i++) { k.bd[i].stride = S.stream[i].stride; k.bd[i].inst = S.stream[i].divisor != 0; }
    for (int i = 0; i < S.nattr; i++) {
        va_st a; memcpy(&a, &S.attr[i], sizeof a); VkFormat vf;
        if (!nvn_fmt_to_vk(a.fmt, &vf, NULL) || vf == VK_FORMAT_UNDEFINED) { skip("a vertex attribute has a format with no Vulkan counterpart", &f_fmt); return 0; }
        if (a.stream >= S.nstream) { skip("a vertex attribute names a stream that is not bound", &f_vattr); return 0; }
        k.at[i].fmt = vf; k.at[i].off = a.offset; k.at[i].binding = (uint8_t)a.stream;
    }
    pipe_entry *pe = get_pipeline(c, &k);
    if (!pe || !pe->pipe) { skip("the pipeline could not be built", &f_pipe); return 0; }

    // dynamic state, every draw (cheap, and a new command buffer starts with none)
    if (P.bound != pe->pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pe->pipe); P.bound = pe->pipe; }
    float vx = 0, vy = 0, vw = (float)P.w, vh = (float)P.h, vn = 0, vf2 = 1;
    if (S.has_vp) { vx = S.vp[0][0]; vy = S.vp[0][1]; vw = S.vp[0][2]; vh = S.vp[0][3]; }
    if (S.has_dr) { vn = S.dr[0]; vf2 = S.dr[1]; }     // the viewport record's own near and far are never set (SetViewport has only x, y, w, h): the depth range is SetDepthRange's, 0..1 by default
    VkViewport v = {vx, vy, vw, vh, vn, vf2};
    if (!ll) { v.y += v.height; v.height = -v.height; }                  // upper-left origin: flip
    VkRect2D sc = {{0, 0}, {P.w, P.h}};
    if (g_sc.has) { int x0 = g_sc.r[0] > 0 ? g_sc.r[0] : 0, y0 = g_sc.r[1] > 0 ? g_sc.r[1] : 0; int x1 = g_sc.r[0] + g_sc.r[2], y1 = g_sc.r[1] + g_sc.r[3];
                    if (x1 > (int)P.w) x1 = (int)P.w; if (y1 > (int)P.h) y1 = (int)P.h; sc = (VkRect2D){{x0, y0}, {x1 > x0 ? (uint32_t)(x1 - x0) : 0, y1 > y0 ? (uint32_t)(y1 - y0) : 0}}; }
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetDepthBias(cb, S.po.units, 0.0f, S.po.factor);
    vkCmdSetBlendConstants(cb, S.blend_color);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_vmask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_vmask[1]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_wmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_wmask[1]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_ref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_ref[1]);

    // vertex buffers
    for (int i = 0; i < S.nstream; i++) {
        VkBuffer b; VkDeviceSize off;
        if (!S.vbuf[i].address || !nvn_vk_pool_buffer(S.vbuf[i].address, S.vbuf[i].size ? S.vbuf[i].size : 1, &b, &off)) { skip("a vertex buffer is not bound or lies outside the memory pools", &f_vbuf); return 0; }
        vkCmdBindVertexBuffers(cb, (uint32_t)i, 1, &b, &off);
    }
    // uniform buffers: each binding of each shader names a uniform slot of its stage; an unbound slot reads zeros
    {
        VkDescriptorBufferInfo bi[32]; VkWriteDescriptorSet wr[32]; int nw = 0;
        const nvn_shader *sh[2] = {vs, fs};
        for (int si = 0; si < 2; si++) {
            int n; const nvn_shbind *b = nvn_shader_binds(sh[si], &n);
            for (int i = 0; i < n && nw < 32; i++) {
                if (b[i].kind != NVN_SB_CBUF || b[i].reg >= 16) continue;
                const p_bind_buf *u = &S.ubo[si][b[i].reg];
                VkBuffer ub; VkDeviceSize uoff;
                if (u->addr && u->size && nvn_vk_pool_buffer(u->addr, u->size, &ub, &uoff)) bi[nw] = (VkDescriptorBufferInfo){ub, uoff, u->size};
                else bi[nw] = (VkDescriptorBufferInfo){zero_buffer(c), 0, 4096};
                wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nw]};
                nw++;
            }
        }
        if (nw) c->CmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pe->layout, 0, (uint32_t)nw, wr);
    }
    *pe_out = pe;
    return 1;
}

void nvn_draw(const p_draw *d) {
    pipe_entry *pe;
    if (d->ind) { static int f; skip("indirect draws are not supported yet", &f); return; }
    if (!prepare(d->prim, &pe)) return;
    vkCmdDraw(nvn_vk_cmd_raw(), (uint32_t)d->count, d->instances ? (uint32_t)d->instances : 1, (uint32_t)d->first, (uint32_t)d->base_instance);
    atomic_fetch_add(&g_ndraw, 1);
}

void nvn_draw_idx(const p_draw_idx *d) {
    static int f_ind, f_byte;
    pipe_entry *pe;
    if (d->ind) { skip("indirect draws are not supported yet", &f_ind); return; }
    if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) { skip("8-bit indices need widening first", &f_byte); return; }
    if (!prepare(d->prim, &pe)) return;
    VkCommandBuffer cb = nvn_vk_cmd_raw(); VkBuffer ib; VkDeviceSize ioff;
    uint64_t isz = d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4;
    if (!nvn_vk_pool_buffer(d->ibuf, isz * (uint64_t)d->count, &ib, &ioff)) { static int f; skip("the index buffer lies outside the memory pools", &f); return; }
    vkCmdBindIndexBuffer(cb, ib, ioff, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, (uint32_t)d->count, d->instances ? (uint32_t)d->instances : 1, 0, d->base_vertex, (uint32_t)d->base_instance);
    atomic_fetch_add(&g_ndraw, 1);
}
