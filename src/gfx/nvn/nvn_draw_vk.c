#include <sys/uio.h>
#include <unistd.h>
#include <signal.h>
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
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);   // nvn_mtl.h: 1 when the range is inside a registered memory pool
#include "nvn_vk.h"
#include "nvn_vk_priv.h"
#include "nvn_shader_vk.h"
#include "../tex/nvn_fmt_vk.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Host time per frame in a draw, split into the parts that could dominate it (reported by nvn_replay_report). Two clock reads per draw cost about 40 ns.
static inline uint64_t dnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
_Atomic uint64_t g_draw_prepare_ns, g_draw_pipe_ns, g_draw_buf_ns, g_draw_pass_ns;   // prepare (whole), its pipeline lookup, its driver buffers, its pass switch

typedef struct nvn_shader nvn_shader;
const nvn_shader *nvn_shader_find(const uint8_t key[8]);
void nvn_shader_alias(const uint8_t key[8], const nvn_shader *s);   // find(key) answers s from now on (a shader found by its code alone, under the program's own key)
int nvn_shader_stage(const nvn_shader *s);
uint32_t nvn_shader_needs(const nvn_shader *s);
const nvn_shbind *nvn_shader_binds(const nvn_shader *s, int *n);
VkDescriptorBufferInfo nvn_shader_c1(const nvn_shader *s);       // the shader's constant segment c[1] as a uniform buffer (nvn_shader_vk.c)
VkShaderModule nvn_shader_module(nvn_shader *s);

nvn_rt_t g_rt;
int nvn_trace_frame_now(void);                   // 1 while the frame NX_DRAW_TRACE names is being replayed
struct nvn_scissor_state g_sc;
static _Atomic uint64_t g_ndraw, g_nskip, g_npipe;
uint64_t nvn_draw_main_target(void);
static _Atomic uint64_t g_main_target; uint64_t nvn_draw_main_target(void) { return atomic_load(&g_main_target); }
uint64_t nvn_draw_count(void) { return atomic_load(&g_ndraw); }
uint64_t nvn_draw_skipped(void) { return atomic_load(&g_nskip); }
uint64_t nvn_pipeline_count(void) { return atomic_load(&g_npipe); }

// ---- state
enum { NTEXSLOT = 256 };                        // DMSH_C2_TEX_SLOTS combined slots, then the separate textures from NVN_SEPTEX_SLOT0
static struct {
    int init;
    const prog_t *prog; int nkeys; uint8_t keys[6][8];
    const prog_t *cprog;                        // the compute program (bound apart from the graphics one)
    const prog_t *vprog, *fprog;                // the programs bound for the vertex and the fragment stage: a title may bind a separable program to each (Super Smash Bros. Ultimate does)
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    uint32_t attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint8_t pushc[0x400]; uint32_t npushc;      // the compute constants pushed so far (nvnCommandBufferPushComputeConstants): the bytes that follow 0x10 in c[0]
    uint32_t img[6][8];                         // the image handle bound to each image slot of each stage (nvnCommandBufferBindImage)
    uint32_t tex[6][NTEXSLOT];                  // the combined handle (texture id | sampler id << 20) bound to each texture slot of each stage
    float vp[16][6]; int has_vp; float dr[2]; int has_dr; p_poly_offset po; float blend_color[4];
    int st_vmask[2], st_wmask[2], st_ref[2];
    int depth_clamp, rast_discard; p_prim_restart restart;
} S;
static _Atomic int g_origin_ll = 1, g_depth_zero;   // NVN's defaults: lower-left origin, -w..w depth
static int g_queue_ll;
extern int dm_switch2_gfx __attribute__((weak));                  // (declared again at its use below)
static uint32_t g_used_streams;                          // the vertex streams the bound attributes read (set by prepare)
static int64_t g_sync_imin, g_sync_imax;                 // the indices (plus base vertex) of the draw being synchronized, for its vertex range check
static size_t g_dbg_size[2];                      // the SPIR-V byte sizes of those two shaders
static uint8_t g_dbg_keys[2][4];                  // the first four bytes of the keys of the vertex and fragment shader the last draw resolved
static int g_base_instance;                      // the draw being prepared: c[0] carries it for the shaders (DMSH_C0_BASE_INSTANCE)

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
    case OP_PUSH_CC: { const uint8_t *q = a; uint32_t o, n; memcpy(&o, q, 4); memcpy(&n, q + 4, 4); if (o <= sizeof S.pushc && n <= sizeof S.pushc - o) { memcpy(S.pushc + o, q + 8, n); if (o + n > S.npushc) S.npushc = o + n;
        if (&dm_switch2_gfx && dm_switch2_gfx && !getenv("NX_PUSHC_RAW"))
            for (uint32_t k = (o + 7) & ~7u; k + 8 <= o + n; k += 8) {       // a Switch 2 compute program stores through a 64-bit buffer address its title pushes (c[0] byte 0x10 + k): the guest's address of a pool becomes the device's
                uint64_t v; memcpy(&v, S.pushc + k, 8);
                if (v < 0x100000000ull || v >= (1ull << 48)) continue;
                uint64_t d = nvn_vk_device_address(v, 1);
                if (d) { memcpy(S.pushc + k, &d, 8); static int lg = -1; static unsigned nl; if (lg < 0) lg = getenv("NX_PUSHC_LOG") != NULL; if (lg && nl++ < 20) fprintf(stderr, "[dm] [nvn] push constant at c[0]+%#x: guest address %#llx -> device %#llx\n", 0x10 + k, (unsigned long long)v, (unsigned long long)d); }
            } } return 1; }
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = a;
        if (b->stages & 0x1f) { S.prog = b->prog; S.nkeys = b->prog ? nvn_prog_keys(b->prog, S.keys) : 0; }
        { static int lg = -1; static unsigned n; if (lg < 0) lg = getenv("NX_SKIP_LOG") != NULL; if (lg && n < 60) { n++; fprintf(stderr, "[dm] [nvn] BindProgram %p stages %#x (%d stage(s) in it)\n", (const void *)b->prog, (unsigned)b->stages, b->prog ? (int)b->prog->nstage : -1); } }
        { static int bl = -2, bf, bt; if (bl == -2) { const char *e = getenv("NX_BINDPROG_LOG"); bl = e && sscanf(e, "%d:%d", &bf, &bt) == 2; }       // NX_BINDPROG_LOG=<from frame>:<to frame>: every BindProgram in those frames, with the keys of its stages
          if (bl > 0) { extern uint64_t nvn_present_count(void); uint64_t fr = nvn_present_count(); if ((int)fr >= bf && (int)fr <= bt) { uint8_t kk[6][8]; int nk = b->prog ? nvn_prog_keys(b->prog, kk) : 0; fprintf(stderr, "[dm] [nvn] frame %llu BindProgram stages %#x:", (unsigned long long)fr, (unsigned)b->stages); for (int q = 0; q < nk; q++) fprintf(stderr, " %02x%02x%02x%02x", kk[q][0], kk[q][1], kk[q][2], kk[q][3]); fprintf(stderr, "\n"); } } }
        if (b->stages & 0x01) S.vprog = b->prog;
        if (b->stages & 0x02) S.fprog = b->prog;                                       // NVNshaderStage: vertex 0, fragment 1, geometry 2, tessellation control 3, evaluation 4, compute 5; the bits are 1 << stage
        if ((b->stages & 0x03) == 0x03) S.vprog = S.fprog = b->prog;
        if (b->stages & 0x20) S.cprog = b->prog;
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
    case OP_BIND_TEX: case OP_BIND_TEX1: {
        p_bind_handles tmp; const p_bind_handles *b = bind_rec(op, a, &tmp);
        for (int i = 0; i < b->n; i++) {
            uint64_t h = b->h[i];
            if (op == OP_BIND_TEX && b->defer && b->src) h = ((const uint64_t *)(uintptr_t)b->src)[i];     // BindTexturesDeferred: the handle is read when the command runs
            int slot = b->first + i;
            if (b->stage >= 0 && b->stage < 6 && slot >= 0 && slot < NTEXSLOT) {
                static int lg = -2, ls, lo, lf, lt; static unsigned nlog;       // NX_TEXBIND_LOG=<stage>:<slot>:<from frame>:<to frame>: every change of that slot's handle in those frames
                if (lg == -2) { const char *e = getenv("NX_TEXBIND_LOG"); lg = e && sscanf(e, "%d:%d:%d:%d", &ls, &lo, &lf, &lt) == 4; }
                if (lg > 0 && b->stage == ls && slot == lo && S.tex[b->stage][slot] != (uint32_t)h && nlog < 200) { extern uint64_t nvn_present_count(void); uint64_t fr = nvn_present_count(); if ((int)fr >= lf && (int)fr <= lt) { nlog++; fprintf(stderr, "[dm] [nvn] texbind frame %llu stage %d slot %d: %#x -> %#llx\n", (unsigned long long)fr, ls, lo, S.tex[b->stage][slot], (unsigned long long)h); } }
                S.tex[b->stage][slot] = (uint32_t)h; }
        }
        return 1; }
    case OP_BIND_IMG: {
        p_bind_handles tmp; const p_bind_handles *b = bind_rec(op, a, &tmp);
        for (int i = 0; i < b->n; i++) { int slot = b->first + i; if (b->stage >= 0 && b->stage < 6 && slot >= 0 && slot < 8) S.img[b->stage][slot] = (uint32_t)b->h[i]; }
        return 1; }
    case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) S.ssbo[b->stage][b->index] = *b; return 1; }
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

typedef struct pipe_entry { pkey key; uint64_t hash; VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSetLayout dsl; int uses_set; struct pipe_entry *next, *hnext; } pipe_entry;
static pipe_entry *g_pipes;
enum { PIPE_BUCKETS = 4096 };                             // a course builds thousands of pipelines, and every draw looks one up: a chain of four at most, not a walk over all of them
static pipe_entry *g_pbuck[PIPE_BUCKETS];
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_since_cut;                                                   // draws made since the last cut of the submission
static _Atomic uint64_t g_nplook, g_nplast, g_npnew;      // lookups, answered by the last-key cache, built (for the per-frame statistics)
void nvn_draw_pipe_stats(uint64_t *look, uint64_t *last, uint64_t *made) {
    *look = atomic_load(&g_nplook); *last = atomic_load(&g_nplast); *made = atomic_load(&g_npnew);
}
static uint64_t pkey_hash(const pkey *k) {                 // over the whole key: a hash collision is still told apart by the memcmp
    uint64_t h = 0x9E3779B97F4A7C15ull; const uint8_t *p = (const uint8_t *)k; size_t n = sizeof *k;
    for (; n >= 8; p += 8, n -= 8) { uint64_t w; memcpy(&w, p, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 31; }
    if (n) { uint64_t w = 0; memcpy(&w, p, n); h = (h ^ w) * 0x100000001B3ull; }
    return h ^ (h >> 29);
}

// The texture handle a shader binding samples: the stage's slot in the driver table, or (bindless) the 32-bit word the game keeps in one of its uniform buffers
static uint32_t bind_handle(int stage, const nvn_shbind *b) {
    if (b->kind != NVN_SB_TEX_UBO) return S.tex[stage][b->reg];
    const p_bind_buf *u = &S.ubo[stage][b->reg & 0xff]; uint32_t off = b->reg >> 8, h = 0;
    if (u->addr && (uint64_t)off + 4 <= u->size) memcpy(&h, (const void *)(uintptr_t)(u->addr + off), 4);
    return h;
}

// The 32 elements of an array binding the shader indexes by pool ID (Ryujinx's fallback arrays, element = min(ID, 31)): element k is the texture (sampler) of pool ID k
static void array_fill(const nvn_shbind *b, VkDescriptorImageInfo *out) {
    for (uint32_t k = 0; k < 32; k++) {
        if (b->kind == NVN_SB_TEXA) { VkImageView v = VK_NULL_HANDLE; nvn_vk_desc_array_tex(k, (int)b->aux, &v); out[k] = (VkDescriptorImageInfo){VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL}; }
        else out[k] = (VkDescriptorImageInfo){nvn_vk_desc_sampler_id(k), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    }
}

static VkPipelineLayout make_layout(vkctx_t *c, const pkey *k, pipe_entry *e) {
    VkDescriptorSetLayoutBinding lb[64]; int nb = 0;
    const nvn_shader *sh[2] = {k->vs, k->fs};
    int narr = 0;                                    // does any binding index a 32-entry texture (or sampler) array?
    for (int si = 0; si < 2; si++) {
        int n; const nvn_shbind *b = nvn_shader_binds(sh[si], &n);
        for (int i = 0; i < n; i++) {
            VkShaderStageFlags st = si ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            int j = 0; for (; j < nb; j++) if (lb[j].binding == b[i].binding) break;
            if (j == nb) { if (nb == 64) continue; VkDescriptorType dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                if (b[i].kind == NVN_SB_IMG) dt = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; else if (b[i].kind == NVN_SB_TEX || b[i].kind == NVN_SB_TEX_UBO) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; else if (b[i].kind == NVN_SB_SBUF) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                if (b[i].kind == NVN_SB_TEXA) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; else if (b[i].kind == NVN_SB_SAMPA) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
                lb[nb++] = (VkDescriptorSetLayoutBinding){b[i].binding, dt, (b[i].kind == NVN_SB_TEXA || b[i].kind == NVN_SB_SAMPA) ? 32u : 1u, 0, NULL}; }
            lb[j].stageFlags |= st;
            if (b[i].kind == NVN_SB_TEXA || b[i].kind == NVN_SB_SAMPA) narr = 1;
        }
    }
    VkDescriptorSetLayout dsl; VkPipelineLayout pl;
    if (narr) {                                       // a push layout cannot carry a 32-element array: go through a normal descriptor set
        VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = 0, .bindingCount = nb, .pBindings = lb};
        if (vkCreateDescriptorSetLayout(c->dev, &di, NULL, &dsl)) return VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
        if (vkCreatePipelineLayout(c->dev, &li, NULL, &pl)) { vkDestroyDescriptorSetLayout(c->dev, dsl, NULL); return VK_NULL_HANDLE; }
        e->uses_set = 1; e->dsl = dsl;
        return pl;
    }
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = nb, .pBindings = lb};
    if (vkCreateDescriptorSetLayout(c->dev, &di, NULL, &dsl)) return VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
    if (vkCreatePipelineLayout(c->dev, &li, NULL, &pl)) { vkDestroyDescriptorSetLayout(c->dev, dsl, NULL); return VK_NULL_HANDLE; }
    e->uses_set = 0; e->dsl = dsl;
    return pl;
}

static pipe_entry *get_pipeline(vkctx_t *c, const pkey *k) {
    static _Thread_local pipe_entry *last;                  // a run of draws of one material (the common case) needs no hash and no lock
    atomic_fetch_add_explicit(&g_nplook, 1, memory_order_relaxed);
    if (last && !memcmp(&last->key, k, sizeof *k)) { atomic_fetch_add_explicit(&g_nplast, 1, memory_order_relaxed); return last; }
    uint64_t h = pkey_hash(k); unsigned b = (unsigned)(h & (PIPE_BUCKETS - 1));
    pthread_mutex_lock(&g_pmu);
    for (pipe_entry *e = g_pbuck[b]; e; e = e->hnext) if (e->hash == h && !memcmp(&e->key, k, sizeof *k)) { pthread_mutex_unlock(&g_pmu); return last = e; }
    pthread_mutex_unlock(&g_pmu);
    pipe_entry *e = calloc(1, sizeof *e); e->key = *k;
    e->layout = make_layout(c, k, e);
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
    pthread_mutex_lock(&g_pmu); e->hash = h; e->hnext = g_pbuck[b]; g_pbuck[b] = e; e->next = g_pipes; g_pipes = e; pthread_mutex_unlock(&g_pmu);
    atomic_fetch_add(&g_npnew, 1);
    return last = e;
}

// ---- the render pass
static struct { int active; nvn_rt_t rt; nvn_attach col[8], dep; int have_dep; uint32_t w, h, layers; VkPipeline bound; } P;

void nvn_draw_pass_end(void) {
    VkCommandBuffer cb = nvn_vk_cmd_raw();
    if (!P.active || !cb) { P.active = 0; return; }
    vkCmdEndRendering(cb);
    P.active = 0;
    nvn_vk_barrier_mark();                 // deferred: what the draws wrote is visible to whatever reads it next
}
int nvn_draw_pass_active(void) { return P.active; }
void nvn_draw_new_submission(void) { P.active = 0; P.bound = VK_NULL_HANDLE; }

static int rt_same(const nvn_rt_t *a, const nvn_rt_t *b) { return !memcmp(a, b, sizeof *a); }

_Atomic uint64_t g_nvn_npass;                             // render passes begun, for the per-frame statistics
static int pass_begin(vkctx_t *c, VkCommandBuffer cb) {
    (void)c; atomic_fetch_add(&g_nvn_npass, 1);
    nvn_attach *col = P.col, *dep = &P.dep; int have_dep = 0;
    VkRenderingAttachmentInfo ca[8], da, sa;
    uint32_t w = 0, h = 0, layers = 1;
    nvn_draw_pass_end();                                          // the targets may differ from the open pass's
    nvn_vk_barrier_flush(cb);                                     // the barrier the closed pass left pending goes before the new pass begins: a barrier cannot be recorded inside one
    { static int dr = -1; if (dr < 0) dr = getenv("NX_DEPTH_READ") != NULL;      // NX_DEPTH_READ=<width> (with NX_CS_READ_DIR, NX_SUBMIT_SYNC=1, a traced frame): the depth attachment of each pass of that width, read as the pass begins
      extern int nvn_trace_frame_now(void); extern int nvn_vk_tex_read_record_w(VkCommandBuffer, uint64_t, const char *, unsigned);
      extern int g_t2t_seen; static unsigned kk; if (dr && g_rt.dser && g_t2t_seen && (kk++ % 6) == 0 && nvn_trace_frame_now()) { char tg[64]; snprintf(tg, sizeof tg, "pass%llu_d%llu", (unsigned long long)atomic_load(&g_nvn_npass), (unsigned long long)g_rt.dser); nvn_vk_tex_read_record_w(cb, g_rt.dser, tg, (unsigned)atoi(getenv("NX_DEPTH_READ"))); nvn_vk_barrier_all(cb); } }
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
// ---- the driver's constant segments and the translator's support buffer: built per draw from the bound state, as nvn_mtl.m encode_args does
enum { SUPPORT_SIZE = 1392 };                                         // the translator's SupportBuffer (std140: alpha test 0, is_bgra 16, viewport inverse 144, size 160, frag scale count 176, render scale 192)
extern int dm_switch2_gfx __attribute__((weak));                  // hle_nvn.c: the title boots nn::gs (a Switch 2 title)
static void put_f(uint8_t *b, size_t off, float v) { memcpy(b + off, &v, 4); }
static void put_u(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }
static void trace_note_sampled(uint64_t s); static void trace_note_prio(uint64_t s);
static uint8_t g_last_push[0x400]; static uint32_t g_last_npush;      // the last block of compute constants a dispatch had
static unsigned g_cs_units, g_cs_rows;                           // the dispatch being prepared (Switch 2): its program's pushed-constant block in 16 byte units and its number of storage buffer rows, from the control section (0 = unknown)
static int32_t g_disp_dim[3];                                     // the dispatch being prepared: c[0] carries its group counts for the shaders (DMSH_C0_DISPATCH)
// A device address that is always safe to read and write: 64 MB of zeros (host memory). An empty or unmapped storage row of a Switch 2 shader points here, so a stray offset reads zeros and a store
// lands where nobody looks, instead of faulting the GPU at address 0 or in a gap between allocations. NX_NO_DUMMY=1 leaves such rows as they were.
static uint64_t dummy_addr(vkctx_t *c) {
    static vkbuf_t zd; static int made;
    if (!made) { made = 1; if (!vkbuf_create(c, &zd, 64u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 1)) memset(zd.map, 0, 64u << 20); }
    return zd.addr;
}
static VkDescriptorBufferInfo driver_buffer(vkctx_t *c, int kind, int stage, float vw, float vh) {
    size_t size = kind == NVN_SB_SUPPORT ? SUPPORT_SIZE : DMSH_DRV_SIZE;
    vkbuf_t b = {0}; VkDescriptorBufferInfo r; void *mp;
    static int own = -1; if (own < 0) own = getenv("NX_C0_OWN") != NULL;       // NX_C0_OWN=1: every driver segment gets a buffer of its own instead of a piece of the transient arena
    if (!own && !nvn_vk_transient(size, &r, &mp)) b.map = mp;                                  // a piece of the transient arena (no allocation per draw)
    else if (vkbuf_create(c, &b, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1)) return (VkDescriptorBufferInfo){zero_buffer(c), 0, 4096};
    else { nvn_vk_keep(&b); r = (VkDescriptorBufferInfo){b.buf, 0, size}; }           // its own buffer, freed once the submission has completed
    uint8_t *m = b.map; memset(m, 0, size);
    if (kind == NVN_SB_SUPPORT) {
        put_f(m, 144, vw > 0 ? 2.0f / vw : 0); put_f(m, 148, vh > 0 ? 2.0f / vh : 0);   // viewport inverse: what a shader with the viewport transform off scales to clip space with
        put_f(m, 160, vw); put_f(m, 164, vh);
        for (int i = 0; i < 73; i++) put_f(m, 192 + 16 * i, 1.0f);                       // no resolution scaling
    } else if (kind == NVN_SB_DRV0) {
        float ps = c->props.limits.pointSizeRange[1]; if (ps > 64.0f) ps = 64.0f;
        put_f(m, DMSH_C0_POINT_SIZE, ps);                                                // the largest point size GLSLC clamps to; 64 on every device that has at least that
        put_u(m, DMSH_C0_BASE_INSTANCE, (uint32_t)g_base_instance);
        if (stage == 5) for (int q = 0; q < 3; q++) put_u(m, DMSH_C0_DISPATCH + 4 * q, (uint32_t)g_disp_dim[q]);
        int sw2cs = stage == 5 && &dm_switch2_gfx && dm_switch2_gfx;                    // a Switch 2 compute program: its pushed constants start at 0x10, the handle table at 0x50, the storage buffer rows (address, size) at 0xc0: the 600 particle programs read their buffers as c[0][0xc0 + 0x10 s]
        size_t tbl = stage == 0 ? DMSH_C0_SSBO_VS : stage == 5 ? (sw2cs ? (size_t)0xc0 : DMSH_C0_SSBO_CS) : DMSH_C0_SSBO_PS;                     // the stage's storage buffers: address and size, 0x10 per slot
        for (int r = 0; r < DMSH_C0_SSBO_SLOTS; r++) {
            uint64_t a = S.ssbo[stage][r].addr;
            // A Switch 2 shader reads these as raw pointers (SM86 LDG through the table's address): they must be device addresses, not the guest's
            if (&dm_switch2_gfx && dm_switch2_gfx && a) {
                uint64_t d = nvn_vk_device_address(a, S.ssbo[stage][r].size ? (uint64_t)S.ssbo[stage][r].size : 1);
                if (d) a = d;
                else {                                                                       // not in any pool (freed, or never mapped): the guest's address would fault the GPU
                    static unsigned nbad;
                    if (nbad++ < 10) fprintf(stderr, "[dm] [RED] [nvn] stage %d storage buffer %d at %#llx (%u bytes) is in no memory pool: a zero page stands in\n", stage, r, (unsigned long long)a, (unsigned)S.ssbo[stage][r].size);
                    uint64_t z = dummy_addr(c); if (z) a = z;
                }
            }
            else if (&dm_switch2_gfx && dm_switch2_gfx && !a && !getenv("NX_NO_DUMMY") && (stage != 5 || r < 11)) { uint64_t z = dummy_addr(c); if (z) a = z; }       // an empty row
            memcpy(m + tbl + (size_t)r * DMSH_C0_ROW, &a, 8);
            put_u(m, tbl + (size_t)r * DMSH_C0_ROW + 8, (uint32_t)S.ssbo[stage][r].size);
        }
        if (sw2cs && S.npushc) memcpy(m + 0x10, S.pushc, S.npushc < DMSH_DRV_SIZE - 0x10 ? S.npushc : DMSH_DRV_SIZE - 0x10);
        if (sw2cs && !getenv("NX_CS_ONE_TABLE")) {                                      // the small programs of the world (91e5d7fe...) read their buffer at c[0][0x20]: a table that starts after the pushed block (0x10 with no push). Both tables are filled; only rows with a buffer, so neither clobbers the other's words
            size_t t2 = 0x10 + (((size_t)S.npushc + 0x3f) & ~(size_t)0x3f);
            if (t2 != tbl && !getenv("NX_NO_DUMMY")) for (int r = 0; r < 4; r++) if (!S.ssbo[stage][r].addr && t2 + 16 * (size_t)(r + 1) <= 0x50) { uint64_t z = dummy_addr(c); if (z) memcpy(m + t2 + (size_t)r * DMSH_C0_ROW, &z, 8); }       // the first rows, below the handle table, point at the dummy too
            if (t2 != tbl) for (int r = 0; r < DMSH_C0_SSBO_SLOTS; r++) if (S.ssbo[stage][r].addr) { uint8_t *dst = m + t2 + (size_t)r * DMSH_C0_ROW; if (dst + 16 <= m + 0xc0 || t2 + 16 * (size_t)r >= 0x170) memcpy(dst, m + tbl + (size_t)r * DMSH_C0_ROW, 16); }
        }
        unsigned cu = g_cs_units; int derived = 0;       // a program whose control gives no push size but has four or more rows (68b1b94c: 116 bytes pushed, ten rows, its pointers at c[0][0x110] and c[0][0x120], the list and the counter of rows 8 and 9): the rows follow the push block, rounded up to 16 bytes
        if (sw2cs && cu == 0 && g_cs_rows >= 4 && S.npushc && !getenv("NX_CS_NO_DERIVE")) { cu = ((unsigned)S.npushc + 15) / 16; derived = 1; }
        if (sw2cs && cu >= 1 && (cu <= 7 || derived) && !getenv("NX_CS_OLD_ROWS")) {   // the program says where its rows are: after a push block of g_cs_units * 0x10 bytes, g_cs_rows rows (then its handle table). 24ff13bd reads them from 0x40, 4dc886e0 from 0x50, 7d3f806b from 0x20
            size_t base = 0x10 + 0x10 * (size_t)cu, end = base + 0x10 * (size_t)(g_cs_rows < 16 ? g_cs_rows : 16);
            if (!derived) memset(m + base, 0, 0x190 - base);                             // whatever the older fills put there was for other programs
            for (size_t r = 0; r < 16 && base + 0x10 * r < end; r++) {
                if (S.ssbo[5][r].addr) memcpy(m + base + 0x10 * r, m + tbl + r * DMSH_C0_ROW, 16);
                else if (!getenv("NX_NO_DUMMY")) { uint64_t z = dummy_addr(c); if (z) memcpy(m + base + 0x10 * r, &z, 8); }
            }
        }
        if (stage == 5 && sw2cs && !S.ssbo[5][0].addr && !S.npushc) { static unsigned nz; if (nz++ < 20) fprintf(stderr, "[dm] [RED] [nvn] a compute dispatch with no storage buffer 0 bound (slot addr %#llx, slot 2 %#llx)\n", (unsigned long long)S.ssbo[5][0].addr, (unsigned long long)S.ssbo[5][2].addr); }
        { static int cd = -1; static unsigned ncd; static unsigned nall; if (cd < 0) cd = getenv("NX_CS_BINDLOG") != NULL; if (cd && stage == 5 && (ncd < 30 || (++nall % 300) == 0)) { ncd++; extern uint64_t nvn_vk_desc_serial(uint32_t); extern int nvn_vk_debug_info(uint64_t, unsigned *, unsigned *, unsigned *, unsigned *, unsigned *);       // NX_CS_BINDLOG=1: what the first 30 dispatches and then every 300th have bound (textures, images: handle, serial, format, size)
            fprintf(stderr, "[dm] [nvn] compute bindings:"); for (int q = 0; q < 8; q++) if (S.tex[5][q] && S.tex[5][q] != 0x10200102) { uint64_t sr = nvn_vk_desc_serial(S.tex[5][q]); unsigned f = 0, w = 0, h = 0, l = 0, d = 0; nvn_vk_debug_info(sr, &f, &w, &h, &l, &d); fprintf(stderr, " t%d=%#x(%llu f%#x %ux%u)", q, S.tex[5][q], (unsigned long long)sr, f, w, h); }
            for (int q = 0; q < 4; q++) if (S.img[5][q]) { uint64_t sr = nvn_vk_desc_serial(S.img[5][q]); unsigned f = 0, w = 0, h = 0, l = 0, d = 0; nvn_vk_debug_info(sr, &f, &w, &h, &l, &d); { extern int nvn_vk_debug_mem(uint64_t, uint64_t *, uint64_t *); uint64_t mm = 0, ms = 0; nvn_vk_debug_mem(sr, &mm, &ms); fprintf(stderr, " i%d=%#x(%llu f%#x %ux%u mem %#llx+%#llx)", q, S.img[5][q], (unsigned long long)sr, f, w, h, (unsigned long long)mm, (unsigned long long)ms); } } fprintf(stderr, " push %u\n", S.npushc); } }
        { static int cl = -1; static unsigned ncl; if (cl < 0) cl = getenv("NX_CS_C0LOG") != NULL; if (cl && stage == 5 && ncl < 14) { ncl++; fprintf(stderr, "[dm] [nvn] compute c0 (sw2 %d, push %u, table %#zx):", sw2cs, S.npushc, tbl); for (int q = 0; q < 0x90; q += 4) { uint32_t v; memcpy(&v, m + q, 4); fprintf(stderr, "%s%08x", q % 16 ? " " : "\n[dm] [nvn]   ", v); } fprintf(stderr, "\n"); } }
    } else {                                                                             // c[2]: the handle of each bound texture slot, one word every DMSH_C2_ROW bytes
        for (int r = 0; r < DMSH_C2_TEX_SLOTS; r++) put_u(m, DMSH_C2_TEX + (size_t)r * DMSH_C2_ROW, S.tex[stage][r]);
        for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) put_u(m, DMSH_C2_SEPTEX + (size_t)r * DMSH_C2_ROW, S.tex[stage][DMSH_SEPTEX_SLOT0 + r]);
    }
    return r;
}
static VkBuffer zero_storage(vkctx_t *c) {                        // what an unbound storage buffer slot reads
    static vkbuf_t z; static int made;
    if (!made) { made = 1; if (!vkbuf_create(c, &z, 65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1)) memset(z.map, 0, 65536); }
    return z.buf;
}
// A uniform buffer as it is when the draw is recorded: copied to the transient arena (the commands run later, and the game may rewrite the memory first). A copy made earlier in the same submission
// is reused while the bytes are unchanged. When the arena is full the draw keeps the live buffer.
// The comparison reads a shadow copy in ordinary cached memory, not the arena: the arena is host-visible device memory, uncached on an integrated GPU, where a CPU read is
// many times slower than a write (thousands of small uniform binds a frame on a Switch 2 title)
static void ubo_snapshot(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    static struct { uint64_t addr, size, fence, cap; VkDescriptorBufferInfo info; uint8_t *shadow; } cache[512];
    uint64_t fence = nvn_vk_next_fence(); unsigned h = (unsigned)((addr >> 4) * 2654435761u) & 511;
    if (cache[h].fence == fence && cache[h].addr == addr && cache[h].size == size && !memcmp(cache[h].shadow, (const void *)(uintptr_t)addr, (size_t)size)) { *out = cache[h].info; return; }
    VkDescriptorBufferInfo ti; void *mp;
    if (nvn_vk_transient((size_t)size, &ti, &mp)) return;
    memcpy(mp, (const void *)(uintptr_t)addr, (size_t)size);
    if (cache[h].cap < size) { uint8_t *n = realloc(cache[h].shadow, (size_t)size); if (!n) { cache[h].fence = 0; *out = ti; return; } cache[h].shadow = n; cache[h].cap = size; }
    memcpy(cache[h].shadow, (const void *)(uintptr_t)addr, (size_t)size);
    cache[h].addr = addr; cache[h].size = size; cache[h].fence = fence; cache[h].info = ti;
    *out = ti;
}
// The shader of one stage of a program in the library, by the program's key for it and then by its code
static const nvn_shader *find_stage(const prog_t *p, int want) {
    if (!p) return NULL;
    uint8_t keys[6][8]; int nk = nvn_prog_keys(p, keys);
    for (int i = 0; i < nk; i++) { const nvn_shader *s = nvn_shader_find(keys[i]); if (s && nvn_shader_stage(s) == want) return s; }
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        uint8_t ak[8]; const nvn_shader *s = prog_alt_key(p, i, ak) ? nvn_shader_find(ak) : NULL;
        if (s && nvn_shader_stage(s) == want) { uint8_t pk[8]; if (prog_key(p, i, pk)) nvn_shader_alias(pk, s); return s; }
    }
    return NULL;
}

// With NX_SHADER_DUMP set, the stages of a program the library lacks are dumped by their code key, once per program
static void dump_missing(const prog_t *p) {
    static int on = -1; if (on < 0) on = getenv("NX_SHADER_DUMP") != NULL;
    if (!on || !p) return;
    static uint64_t seen[512]; unsigned h = (unsigned)(p->serial & 511);
    if (seen[h] == p->serial + 1) return;
    seen[h] = p->serial + 1;
    for (unsigned i = 0; i < p->nstage && i < 6; i++) { uint8_t ak[8]; if (prog_alt_key(p, i, ak) && !nvn_shader_find(ak)) nvn_dump_stage(p, i); }
}

static void skip(const char *why, int *flag) {
    unsigned k = atomic_fetch_add(&g_nskip, 1);
    { static int lg = -1; if (lg < 0) lg = getenv("NX_SKIP_LOG") != NULL;   // NX_SKIP_LOG=1: every skipped draw with its reason, 1 in 64 after the first 400
      if (lg && (k < 400 || k % 64 == 0)) fprintf(stderr, "[dm] [nvn] skip #%u: %s\n", k, why); }
    if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) fprintf(stderr, "[dm] [RED] [nvn] draw skipped: %s\n", why);
    if (nvn_trace_frame_now()) {      // the traced frame: every skip, with the target it would have drawn into and the program's keys
        fprintf(stderr, "[dm] [nvn] trace skip: colour %llu depth %llu keys", (unsigned long long)g_rt.cser[0], (unsigned long long)g_rt.dser);
        for (int i = 0; i < S.nkeys; i++) { fprintf(stderr, " "); for (int b = 0; b < 8; b++) fprintf(stderr, "%02x", S.keys[i][b]); }
        fprintf(stderr, ": %s\n", why);
    }
}

// A vertex attribute's format: the texture table (nvn_fmt_to_vk) leaves the three-component formats out because sampling them is not universal, but Vulkan takes them as vertex input
static int vertex_fmt(int f, VkFormat *vf) {
    static const VkFormat rgb[] = {   // NVN_FORMAT_RGB8 .. RGB32I = 0x19 .. 0x24
        VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT,
        VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT,
        VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT};
    static const VkFormat i2f[] = {   // NVN_FORMAT_R8_UI2F .. RGB10A2_I2F = 0x51 .. 0x6C: integers read as floats (scaled); 32-bit ones have no Vulkan counterpart
        VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, 0, 0,
        VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, 0, 0,
        VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED, 0, 0,
        VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED, 0, 0,
        VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32};
    if (f >= 0x19 && f <= 0x24) { *vf = rgb[f - 0x19]; return 1; }
    if (f >= 0x51 && f <= 0x6C && i2f[f - 0x51]) { *vf = i2f[f - 0x51]; return 1; }
    return nvn_fmt_to_vk(f, vf, NULL) && *vf != VK_FORMAT_UNDEFINED;
}

// Everything shared by the plain and the indexed draw: the pipeline, the pass, the dynamic state, the vertex buffers and the uniforms. 0 when the draw cannot be made.
static int prepare(unsigned prim, pipe_entry **pe_out) {
    static int f_ntex, f_tex, f_needs, f_noprog, f_noshader, f_prim, f_vbuf, f_vattr, f_nort, f_pipe, f_fmt, f_off;
    state_init();
    {   // NX_CUT=<n> (default 0 = off): after n draws, before a new render pass, the recorded work goes to the GPU (mtl_sub_cut), so GPU and host overlap. Every submission costs the driver a walk over all its memory objects: on the Radeon a cut of 64 made the race frame 40 ms against 30 without, on the RTX it changed the median little and the 95th percentile a lot (60 ms against 47)
        static int cut = -1; if (cut < 0) cut = getenv("NX_CUT") ? atoi(getenv("NX_CUT")) : 0;
        extern void mtl_sub_cut(void);
        if (cut > 0 && g_since_cut >= (unsigned)cut && (!P.active || !rt_same(&P.rt, &g_rt))) { mtl_sub_cut(); g_since_cut = 0; }
    }
    vkctx_t *c = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd_raw();
    if (!c || !cb) return 0;
    if (!P.active) nvn_vk_barrier_flush(cb);                      // never inside an open pass
    if (!c->has_push_descriptor) { skip("the device lacks VK_KHR_push_descriptor", &f_off); return 0; }
    if (!S.prog || !S.nkeys) {
        static int lg = -1, nlog; if (lg < 0) lg = getenv("NX_SKIP_LOG") != NULL;
        if (lg && S.prog && nlog++ < 8) {                                                // NX_SKIP_LOG: what the stages of a program without keys hold (a Switch 2 control section the title never filled?)
            fprintf(stderr, "[dm] [nvn] program %p without keys: %u stage(s)", (const void *)S.prog, S.prog->nstage);
            for (unsigned i = 0; i < S.prog->nstage && i < 6; i++) { const NVNshaderData *d = &S.prog->sd[i]; fprintf(stderr, " | stage %u control %p data %#llx:", i, d->control, (unsigned long long)d->data);
                if (d->control) { const uint32_t *w = d->control; for (int q = 0; q < 16; q++) fprintf(stderr, " %08x", w[q]); } }
            fprintf(stderr, "\n"); }
        skip("no program is bound", &f_noprog); return 0; }
    const nvn_shader *vs = NULL, *fs = NULL; int focus = 0;
    int split = S.vprog && S.fprog && S.vprog != S.fprog;        // a separable program for each stage: each one's shader of its own stage
    if (split) { vs = find_stage(S.vprog, NVN_STAGE_VERTEX); fs = find_stage(S.fprog, NVN_STAGE_FRAGMENT); }
    for (int i = 0; !split && i < S.nkeys; i++) {
        const nvn_shader *s = nvn_shader_find(S.keys[i]);
        if (!s) {
            static int lg = -1; static uint64_t seen[256]; static unsigned nseen; if (lg < 0) lg = getenv("NX_SKIP_LOG") != NULL;
            uint64_t k; memcpy(&k, S.keys[i], 8);
            if (lg && nseen < 256) { unsigned j = 0; while (j < nseen && seen[j] != k) j++; if (j == nseen) { seen[nseen++] = k;   // NX_SKIP_LOG: each key the library lacks, once
                fprintf(stderr, "[dm] [nvn] library lacks shader key "); for (int b = 0; b < 8; b++) fprintf(stderr, "%02x", S.keys[i][b]); fprintf(stderr, " (stage slot %d)\n", i); } }
            continue;
        }
        int st = nvn_shader_stage(s);
        if (st == NVN_STAGE_VERTEX) vs = s; else if (st == NVN_STAGE_FRAGMENT) fs = s;
        else { skip("geometry, tessellation and compute stages are not supported in a draw yet", &f_noshader); return 0; }
    }
    { extern const uint8_t *nvn_shader_keyof(const nvn_shader *); if (vs && fs) { memcpy(g_dbg_keys[0], nvn_shader_keyof(vs), 4); memcpy(g_dbg_keys[1], nvn_shader_keyof(fs), 4); extern size_t nvn_shader_bytes(const nvn_shader *); g_dbg_size[0] = nvn_shader_bytes(vs); g_dbg_size[1] = nvn_shader_bytes(fs); } }
    if (!split && (!vs || !fs)) {           // the normal key hashes bytes that change from run to run: look the stages up by their code (cached per program: it hashes the code area)
        static _Thread_local struct { uint64_t serial; unsigned st; const nvn_shader *s; int valid; } cache[1024];
        for (unsigned i = 0; i < S.prog->nstage && i < 6; i++) {
            typeof(cache[0]) *ce = &cache[(S.prog->serial * 6 + i) & 1023];
            if (!(ce->valid && ce->serial == S.prog->serial && ce->st == i)) {
                uint8_t ak[8]; ce->serial = S.prog->serial; ce->st = i; ce->valid = 1;
                ce->s = prog_alt_key(S.prog, i, ak) ? nvn_shader_find(ak) : NULL;
            }
            if (!ce->s) continue;
            { uint8_t pk[8]; if (prog_key(S.prog, i, pk)) nvn_shader_alias(pk, ce->s); }   // the program's own key finds it directly from now on
            int st = nvn_shader_stage(ce->s);
            if (st == NVN_STAGE_VERTEX && !vs) vs = ce->s; else if (st == NVN_STAGE_FRAGMENT && !fs) fs = ce->s;
        }
    }
    { extern const uint8_t *nvn_shader_keyof(const nvn_shader *); if (vs && fs) { memcpy(g_dbg_keys[0], nvn_shader_keyof(vs), 4); memcpy(g_dbg_keys[1], nvn_shader_keyof(fs), 4); extern size_t nvn_shader_bytes(const nvn_shader *); g_dbg_size[0] = nvn_shader_bytes(vs); g_dbg_size[1] = nvn_shader_bytes(fs); } }       // after the by-code lookup too: a Switch 2 program is found only there
    if (!vs || !fs) {
        dump_missing(S.prog); if (S.vprog != S.prog) dump_missing(S.vprog); if (S.fprog != S.prog && S.fprog != S.vprog) dump_missing(S.fprog);
        static int lg = -1; static unsigned nlog; if (lg < 0) lg = getenv("NX_SKIP_LOG") != NULL;
        if (lg && nlog++ < 1600 && nlog % 16 == 1) {                                     // NX_SKIP_LOG: the state of each stage of the program (the keys found, and what a stage without one holds)
            fprintf(stderr, "[dm] [nvn] no vs/fs pair: %d stage(s), %d key(s), vs %p fs %p", S.prog->nstage, S.nkeys, (const void *)vs, (const void *)fs);
            { uint8_t pk[6][8]; int npk = nvn_prog_keys(S.prog, pk); fprintf(stderr, " | keys"); for (int q = 0; q < npk; q++) { fprintf(stderr, " "); for (int b = 0; b < 8; b++) fprintf(stderr, "%02x", pk[q][b]); } }
            for (unsigned i = 0; i < S.prog->nstage && i < 6; i++) { const NVNshaderData *d = &S.prog->sd[i]; uint32_t nc = 0, c0 = 0, c30 = 0, mg = 0;
                if (d->control) { memcpy(&nc, (const uint8_t *)d->control + 0x704, 4); memcpy(&mg, d->control, 4); }
                void *mb; uint64_t mo; int mapped = d->data && nc && nc < (1u << 24) && mtl_addr(d->data, nc, &mb, &mo);
                if (mapped) { const uint32_t *c = (const uint32_t *)(uintptr_t)d->data; c0 = c[0]; c30 = nc >= 0x34 ? c[0x30 / 4] : 0; }
                uint8_t ak[8] = {0}; int hasak = prog_alt_key(S.prog, i, ak);
                fprintf(stderr, " | stage %u: ncode %#x magic %#x mapped %d c0 %#x sph0 %#x altkey ", i, nc, mg, mapped, c0, c30);
                if (hasak) for (int b = 0; b < 8; b++) fprintf(stderr, "%02x", ak[b]); else fprintf(stderr, "none"); }
            fprintf(stderr, "\n"); }
        skip("the program's shaders are not in the registry (the translated shader library is not loaded)", &f_noshader); return 0; }
    if (nvn_shader_needs(vs) | nvn_shader_needs(fs)) {
        // not an error of the game's: the shader reads something this draw path cannot supply yet (see NVN_NEED_*)
        char why[160]; uint32_t n = nvn_shader_needs(vs) | nvn_shader_needs(fs);
        snprintf(why, sizeof why, "the shader needs%s%s%s%s%s%s%s (not supported yet)", n & NVN_NEED_C0 ? " driver segment c0," : "", n & NVN_NEED_C1 ? " constant segment c1," : "",
                 n & NVN_NEED_C2 ? " driver segment c2," : "", n & NVN_NEED_SBUF ? " storage buffers," : "", n & NVN_NEED_TEX ? " textures," : "", n & NVN_NEED_IMG ? " images," : "", n & NVN_NEED_GEOM ? " geometry helper buffers," : "");
        skip(why, &f_needs); return 0;
    }
    // Textures first: filling a dirty one copies into its image, which cannot happen inside a render pass (this ends the pass; it is opened below)
    VkDescriptorImageInfo ti[32], ii[8]; uint32_t tbind[32], ibind[8]; int nt = 0, ni = 0;
    VkDescriptorImageInfo arr[8][32]; uint32_t abind[8]; VkDescriptorType atype[8]; int na = 0;      // the 32-element texture and sampler arrays a shader indexes by pool ID (array_fill)
    {
        const nvn_shader *sh[2] = {vs, fs};
        for (int si = 0; si < 2; si++) {
            int n; const nvn_shbind *b = nvn_shader_binds(sh[si], &n);
            for (int i = 0; i < n; i++) {
                if (b[i].kind == NVN_SB_IMG) {
                    VkFormat ifmt;
                    if (b[i].reg >= 8 || ni == 8 || !nvn_vk_desc_storage(S.img[si][b[i].reg], &ii[ni], &ifmt)) {
                        char why[160]; snprintf(why, sizeof why, "image slot %u (handle %#x) has no storage image the shader can use", b[i].reg, S.img[si][b[i].reg & 7]); skip(why, &f_tex); return 0; }
                    ibind[ni++] = b[i].binding; continue; }
                if (b[i].kind == NVN_SB_TEXA || b[i].kind == NVN_SB_SAMPA) {
                    if (na == 8) { skip("a pipeline has more than eight texture or sampler arrays", &f_ntex); return 0; }
                    array_fill(&b[i], arr[na]); abind[na] = b[i].binding; atype[na] = b[i].kind == NVN_SB_TEXA ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER; na++; continue; }
                if (b[i].kind != NVN_SB_TEX && b[i].kind != NVN_SB_TEX_UBO) continue;
                const uint32_t th = bind_handle(si, &b[i]);
                if ((b[i].kind == NVN_SB_TEX && b[i].reg >= NTEXSLOT) || nt == 32) { skip("a shader samples more textures than the draw path binds", &f_ntex); return 0; }
                { static int dd = -1; static unsigned cnt; if (dd < 0) dd = getenv("NX_DRAW_DETAIL") != NULL;
                  if (dd && cnt < 24) { cnt++; extern uint64_t nvn_vk_desc_serial(uint32_t); extern long nvn_vk_debug_lit(uint64_t, size_t *, const char *); uint64_t sr = nvn_vk_desc_serial(th); size_t tot = 0; char pn[300]; const char *pre = getenv("NX_FRAME_DUMP"); snprintf(pn, sizeof pn, "%s.tex%llu.ppm", pre ? pre : "nvn_draw", (unsigned long long)sr); long lit = sr ? nvn_vk_debug_lit(sr, &tot, pn) : -2;
                    fprintf(stderr, "[dm] [nvn] draw samples handle %#x (texture id %u, sampler %u) serial %llu: %ld of %zu bytes not zero\n", th, th & 0xfffff, th >> 20, (unsigned long long)sr, lit, tot); } }
                int r = nvn_vk_desc_sampled(th, (int)b[i].aux, &ti[nt]);
                { static long fser = -2; if (fser == -2) fser = getenv("NX_DRAW_SERIAL") ? atol(getenv("NX_DRAW_SERIAL")) : -1;   // NX_DRAW_SERIAL=<texture serial>: details of the draws that sample it
                  extern uint64_t nvn_vk_desc_serial(uint32_t); if (fser >= 0 && nvn_vk_desc_serial(th) == (uint64_t)fser) { focus = 1;
                    fprintf(stderr, "[dm] [nvn] focus draw: stage %d slot %u handle %#x sampler type %#x result %d, prim %u, vs %p fs %p keys %016llx %016llx\n", si, b[i].reg, th, b[i].aux, r, prim, (const void *)vs, (const void *)fs, (unsigned long long)__builtin_bswap64(*(const uint64_t *)vs), (unsigned long long)__builtin_bswap64(*(const uint64_t *)fs)); } }
                { static int dd2 = -1; static unsigned cnt2; if (dd2 < 0) dd2 = getenv("NX_DRAW_DETAIL") != NULL;       // the same texture read again once the draw path has made it ready (the GPU copy)
                  if (dd2 && cnt2 < 12) { cnt2++; extern uint64_t nvn_vk_desc_serial(uint32_t); extern long nvn_vk_debug_lit(uint64_t, size_t *, const char *); extern int mtl_tex_gpu_owned(uint64_t); uint64_t sr = nvn_vk_desc_serial(th); size_t tot = 0; long lit = sr ? nvn_vk_debug_lit(sr, &tot, NULL) : -2;
                    fprintf(stderr, "[dm] [nvn] after sampling: serial %llu result %d gpu-owned %d: %ld of %zu bytes not zero\n", (unsigned long long)sr, r, sr ? mtl_tex_gpu_owned(sr) : -1, lit, tot); } }
                if (r != 1) {
                    char why[160]; uint32_t h = th;
                    snprintf(why, sizeof why, r == 3 ? "texture slot %u (id %u) is not the type the shader samples (type %#x)" : "texture slot %u (id %u) has no image the shader can sample and no stand-in for its type %#x", b[i].reg, h & 0xfffff, (unsigned)b[i].aux);
                    skip(why, &f_tex); return 0;
                }
                tbind[nt++] = b[i].binding;
            }
        }
    }
    VkPrimitiveTopology topo;
    if (!topology(prim, &topo, c->enabled.geometryShader)) { skip("this primitive type needs an index list built for it", &f_prim); return 0; }
    if (!g_rt.n && !g_rt.dser) { skip("no render target is set", &f_nort); return 0; }

    pkey k; memset(&k, 0, sizeof k);
    k.vs = vs; k.fs = fs; k.topo = (uint8_t)topo; k.restart = S.restart.on && is_strip(topo);
    // The pass first: it decides the attachment formats the pipeline is built for
    if (!P.active || !rt_same(&P.rt, &g_rt)) { uint64_t tq = dnow(); if (!pass_begin(c, cb)) { skip("a render target could not be attached", &f_pipe); return 0; } atomic_fetch_add(&g_draw_pass_ns, dnow() - tq); }
    k.ncolor = (uint8_t)g_rt.n;
    for (int i = 0; i < g_rt.n; i++) k.cfmt[i] = g_rt.cser[i] ? P.col[i].fmt : VK_FORMAT_UNDEFINED;
    if (P.have_dep) {
        k.dfmt = (P.dep.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? P.dep.fmt : VK_FORMAT_UNDEFINED;
        k.sfmt = (P.dep.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? P.dep.fmt : VK_FORMAT_UNDEFINED;
    }
    int ll = atomic_load(&g_origin_ll);
    k.cull = S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : S.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : 0;
    { static int no_cull = -1; if (no_cull < 0) no_cull = getenv("NX_NO_CULL") != NULL; if (no_cull) k.cull = 0; }       // a test: draw both faces
    k.front_ccw = (S.poly.front == NVN_FRONT_FACE_CCW) ^ (ll != 0);       // see the conventions at the top
    k.polymode = S.poly.mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : S.poly.mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    if (k.polymode != VK_POLYGON_MODE_FILL && !c->enabled.fillModeNonSolid) k.polymode = VK_POLYGON_MODE_FILL;
    k.depth_clamp = S.depth_clamp && c->enabled.depthClamp;
    k.bias_en = (S.poly.offset_en >> (S.poly.mode == NVN_POLYGON_MODE_POINT ? 0 : S.poly.mode == NVN_POLYGON_MODE_LINE ? 1 : 2)) & 1;
    k.discard = S.rast_discard != 0;
    k.neg1 = !atomic_load(&g_depth_zero);
    int has_depth = k.dfmt != VK_FORMAT_UNDEFINED, has_stencil = k.sfmt != VK_FORMAT_UNDEFINED;
    k.dt = S.ds.dt && has_depth; k.dw = S.ds.dw && has_depth; k.dfunc = (uint8_t)cmp(S.ds.df); { static int nz = -1; if (nz < 0) nz = getenv("NX_NO_ZTEST") != NULL; if (nz) { k.dt = 0; k.dw = 0; } }       /* NX_NO_ZTEST: a test, no depth test or write */ { static int eq = -1; if (eq < 0) eq = getenv("NX_DEPTH_EQUAL_LEQUAL") != NULL; if (eq && k.dfunc == VK_COMPARE_OP_EQUAL) k.dfunc = VK_COMPARE_OP_LESS_OR_EQUAL; }   // NX_DEPTH_EQUAL_LEQUAL=1: a depth-equal colour pass tests less-or-equal (experiment)
    k.st_en = S.ds.st && has_stencil;
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
    // A disabled attribute (format NONE) that comes before an enabled one keeps its location as a one-float read of the first enabled attribute's stream (no shader reads it); trailing ones are dropped,
    // so a draw whose attributes are all off (positions made in the vertex shader) needs no vertex buffer
    int nat = S.nattr, first_stream = -1;
    while (nat > 0) { va_st t; memcpy(&t, &S.attr[nat - 1], sizeof t); if (t.fmt) break; nat--; }
    for (int i = 0; i < nat; i++) { va_st t; memcpy(&t, &S.attr[i], sizeof t); if (t.fmt) { first_stream = t.stream; break; } }
    k.nattr = (uint8_t)nat;
    uint32_t used_streams = 0;
    for (int i = 0; i < nat; i++) {
        va_st a; memcpy(&a, &S.attr[i], sizeof a); VkFormat vf;
        if (a.fmt == 0) { a.fmt = NVN_FORMAT_R32F; a.stream = (uint32_t)first_stream; a.offset = 0; }
        else used_streams |= 1u << a.stream;
        if (!vertex_fmt(a.fmt, &vf)) { { char why[120]; snprintf(why, sizeof why, "a vertex attribute has a format with no Vulkan counterpart (NVN format %#x)", (unsigned)a.fmt); skip(why, &f_fmt); return 0; } }
        if (a.stream >= S.nstream) { skip("a vertex attribute names a stream that is not bound", &f_vattr); return 0; }
        g_used_streams = used_streams;
        { static int i2f = -1; if (i2f < 0) i2f = getenv("NX_ATTR_INT2F") != NULL;       // NX_ATTR_INT2F=1 (diagnosis): an integer attribute is fetched as its value converted to float, which is what the translator's float input declares
          if (i2f) switch (vf) {
              case VK_FORMAT_R8_UINT: vf = VK_FORMAT_R8_USCALED; break; case VK_FORMAT_R8G8_UINT: vf = VK_FORMAT_R8G8_USCALED; break; case VK_FORMAT_R8G8B8_UINT: vf = VK_FORMAT_R8G8B8_USCALED; break; case VK_FORMAT_R8G8B8A8_UINT: vf = VK_FORMAT_R8G8B8A8_USCALED; break;
              case VK_FORMAT_R8_SINT: vf = VK_FORMAT_R8_SSCALED; break; case VK_FORMAT_R8G8_SINT: vf = VK_FORMAT_R8G8_SSCALED; break; case VK_FORMAT_R8G8B8_SINT: vf = VK_FORMAT_R8G8B8_SSCALED; break; case VK_FORMAT_R8G8B8A8_SINT: vf = VK_FORMAT_R8G8B8A8_SSCALED; break;
              case VK_FORMAT_R16_UINT: vf = VK_FORMAT_R16_USCALED; break; case VK_FORMAT_R16G16_UINT: vf = VK_FORMAT_R16G16_USCALED; break; case VK_FORMAT_R16G16B16A16_UINT: vf = VK_FORMAT_R16G16B16A16_USCALED; break;
              case VK_FORMAT_R16_SINT: vf = VK_FORMAT_R16_SSCALED; break; case VK_FORMAT_R16G16_SINT: vf = VK_FORMAT_R16G16_SSCALED; break; case VK_FORMAT_R16G16B16A16_SINT: vf = VK_FORMAT_R16G16B16A16_SSCALED; break;
              default: break; } }
        k.at[i].fmt = vf; k.at[i].off = a.offset; k.at[i].binding = (uint8_t)a.stream;
    }
    uint64_t tp = dnow();
    pipe_entry *pe = get_pipeline(c, &k);
    atomic_fetch_add(&g_draw_pipe_ns, dnow() - tp);
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
    { static int dd4 = -1; static unsigned cnt4; if (dd4 < 0) dd4 = getenv("NX_DRAW_DETAIL") != NULL;
      if (dd4 && cnt4 < 16) { cnt4++; fprintf(stderr, "[dm] [nvn] viewport %g %g %g x %g depth %g..%g scissor %d,%d %ux%u (pass %ux%u, has_vp %d, has scissor %d)\n", v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth, sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height, P.w, P.h, S.has_vp, g_sc.has); } }
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetDepthBias(cb, S.po.units, 0.0f, S.po.factor);
    vkCmdSetBlendConstants(cb, S.blend_color);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_vmask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_vmask[1]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_wmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_wmask[1]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_ref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_ref[1]);

    // vertex buffers
    static _Atomic unsigned nfocus;
    if (focus && atomic_fetch_add(&nfocus, 1) < 6) {
        fprintf(stderr, "[dm] [nvn] focus: viewport %g %g %g x %g, %d streams, %d attributes:", v.x, v.y, v.width, v.height, S.nstream, S.nattr);
        for (int a = 0; a < S.nattr; a++) { va_st va; memcpy(&va, &S.attr[a], sizeof va); fprintf(stderr, " [loc %d fmt %#x off %u stream %u]", a, va.fmt, (unsigned)va.offset, (unsigned)va.stream); }
        fprintf(stderr, "\n");
        for (int i = 0; i < S.nstream; i++) if (S.vbuf[i].address) { const uint32_t *w = (const uint32_t *)(uintptr_t)S.vbuf[i].address;
            fprintf(stderr, "[dm] [nvn] focus stream %d stride %u size %llu:", i, S.stream[i].stride, (unsigned long long)S.vbuf[i].size); for (int q = 0; q < 24; q++) fprintf(stderr, " %08x", w[q]); fprintf(stderr, "\n"); }
        for (int si = 0; si < 2; si++) for (int r = 0; r < 4; r++) if (S.ubo[si][r].addr) { const uint32_t *w = (const uint32_t *)(uintptr_t)S.ubo[si][r].addr;
            fprintf(stderr, "[dm] [nvn] focus ubo stage %d slot %d size %llu:", si, r, (unsigned long long)S.ubo[si][r].size); for (int q = 0; q < 24 && q * 4 < (int)S.ubo[si][r].size; q++) fprintf(stderr, " %08x", w[q]); fprintf(stderr, "\n"); }
    }
    for (int i = 0; i < S.nstream; i++) {
        VkBuffer b; VkDeviceSize off;
        { static int dd3 = -1; static unsigned cnt3; if (dd3 < 0) dd3 = getenv("NX_DRAW_DETAIL") != NULL;
          if (dd3 && cnt3 < 12 && (used_streams >> i & 1)) { cnt3++; const float *f = (const float *)(uintptr_t)S.vbuf[i].address; fprintf(stderr, "[dm] [nvn] vertex stream %d: addr %#llx size %llu stride %u: %g %g %g %g | %g %g %g %g | attrs %d:", i, (unsigned long long)S.vbuf[i].address, (unsigned long long)S.vbuf[i].size, S.stream[i].stride, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], S.nattr);
            for (int a = 0; a < S.nattr; a++) { va_st v; memcpy(&v, &S.attr[a], sizeof v); fprintf(stderr, " [loc %d fmt %#x off %u stream %u]", a, v.fmt, (unsigned)v.offset, (unsigned)v.stream); } fprintf(stderr, "\n"); } }
        if (!(used_streams >> i & 1)) continue;                                  // no attribute reads this stream
        if (!S.vbuf[i].address || !nvn_vk_pool_buffer(S.vbuf[i].address, S.vbuf[i].size ? S.vbuf[i].size : 1, &b, &off)) { skip("a vertex buffer is not bound or lies outside the memory pools", &f_vbuf); return 0; }
        vkCmdBindVertexBuffers(cb, (uint32_t)i, 1, &b, &off);
    }
    // uniform buffers: each binding of each shader names a uniform slot of its stage; an unbound slot reads zeros
    {
        VkDescriptorBufferInfo bi[64]; VkWriteDescriptorSet wr[64]; int nw = 0;
        const nvn_shader *sh[2] = {vs, fs};
        for (int si = 0; si < 2; si++) {
            int n; const nvn_shbind *b = nvn_shader_binds(sh[si], &n);
            for (int i = 0; i < n && nw < 64; i++) {
                if (b[i].kind == NVN_SB_SUPPORT || b[i].kind == NVN_SB_DRV0 || b[i].kind == NVN_SB_DRV2) {
                    uint64_t tb = dnow();
                    bi[nw] = driver_buffer(c, (int)b[i].kind, si, vw, vh);
                    atomic_fetch_add(&g_draw_buf_ns, dnow() - tb);
                    wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nw]};
                    nw++; continue;
                }
                if (b[i].kind == NVN_SB_C1) {
                    bi[nw] = nvn_shader_c1(sh[si]);
                    if (!bi[nw].buffer) bi[nw] = (VkDescriptorBufferInfo){zero_buffer(c), 0, 4096};
                    wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nw]};
                    nw++; continue;
                }
                if (b[i].kind == NVN_SB_SBUF && b[i].reg < 16) {
                    const p_bind_buf *u = &S.ssbo[si][b[i].reg];
                    VkBuffer sbuf; VkDeviceSize soff;
                    if (u->addr && u->size && nvn_vk_pool_buffer(u->addr, u->size, &sbuf, &soff)) bi[nw] = (VkDescriptorBufferInfo){sbuf, soff, u->size};
                    else bi[nw] = (VkDescriptorBufferInfo){zero_storage(c), 0, 4096};
                    wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[nw]};
                    nw++; continue;
                }
                if (b[i].kind != NVN_SB_CBUF || b[i].reg >= 16) continue;
                const p_bind_buf *u = &S.ubo[si][b[i].reg];
                { extern int nvn_trace_frame_now(void); if (nvn_trace_frame_now()) fprintf(stderr, "[dm] [nvn]   uniform block of stage %d: shader binding %u reads NVN binding %u: addr %#llx size %llu\n", si, (unsigned)b[i].binding, (unsigned)b[i].reg, (unsigned long long)u->addr, (unsigned long long)u->size); }
                { static int dd = -1; static unsigned cnt; if (dd < 0) dd = getenv("NX_DRAW_DETAIL") != NULL;       // the first 30 uniform bindings: what is bound and its first four words
                  if (dd && cnt < 30) { cnt++; const float *f = (const float *)(uintptr_t)u->addr; if (si == 0 && cnt < 3) { fprintf(stderr, "[dm] [nvn] vertex UBO floats:"); for (int q = 0; q < 36 && q * 4 < (int)u->size; q++) fprintf(stderr, "%s%g", q % 4 ? " " : " |", ((const float *)(uintptr_t)u->addr)[q]); fprintf(stderr, "\n"); }
                  fprintf(stderr, "[dm] [nvn] UBO stage %d slot %u: addr %#llx size %llu%s %g %g %g %g\n", si, b[i].reg, (unsigned long long)u->addr, (unsigned long long)u->size, u->addr ? " data" : " (unbound)", u->addr ? f[0] : 0, u->addr ? f[1] : 0, u->addr ? f[2] : 0, u->addr ? f[3] : 0); } }
                VkBuffer ub; VkDeviceSize uoff;
                if (u->addr && u->size && nvn_vk_pool_buffer(u->addr, u->size, &ub, &uoff)) {
                    bi[nw] = (VkDescriptorBufferInfo){ub, uoff, u->size};
                    if (&dm_switch2_gfx && dm_switch2_gfx && u->size <= 0x10000 && !getenv("NX_UBO_LIVE")) ubo_snapshot(u->addr, u->size, &bi[nw]);       // a Switch 2 title rewrites its constants while the GPU still has the frame to run: the draw gets the bytes of its own time
                } else bi[nw] = (VkDescriptorBufferInfo){zero_buffer(c), 0, 4096};
                wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nw]};
                nw++;
            }
        }
        for (int i = 0; i < ni && nw < 64; i++)
            wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = ibind[i], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[i]};
        for (int i = 0; i < nt && nw < 64; i++)
            wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = tbind[i], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ti[i]};
        for (int i = 0; i < na && nw < 64; i++)
            wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = abind[i], .descriptorCount = 32, .descriptorType = atype[i], .pImageInfo = arr[i]};
        if (pe->uses_set) {                                               // a normal set, one per draw: the commands run later, so a set the next draw rewrites would show it the last contents
            VkDescriptorSet ds = nvn_vk_alloc_set(pe->dsl);
            if (!ds) { skip("no descriptor set for a draw that indexes a texture array", &f_pipe); return 0; }
            for (int i = 0; i < nw; i++) wr[i].dstSet = ds;
            vkUpdateDescriptorSets(c->dev, (uint32_t)nw, wr, 0, NULL);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pe->layout, 0, 1, &ds, 0, NULL);
        } else if (nw) c->CmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pe->layout, 0, (uint32_t)nw, wr);
    }
    *pe_out = pe;
    return 1;
}


// ---- compute: a dispatch runs the compute program bound with the textures, uniform and storage buffers of stage 5. Its pipelines are kept by shader; the layout is a push descriptor set like a draw's.
static struct cpipe { const nvn_shader *sh; VkPipeline pipe; VkPipelineLayout layout; } g_cp[128]; static int g_ncp;
static struct cpipe *compute_pipeline(vkctx_t *c, const nvn_shader *sh) {
    for (int i = 0; i < g_ncp; i++) if (g_cp[i].sh == sh) return &g_cp[i];
    if (g_ncp == 128) return NULL;
    VkDescriptorSetLayoutBinding lb[32]; int nb = 0; int n; const nvn_shbind *b = nvn_shader_binds(sh, &n);
    for (int i = 0; i < n; i++) {
        int j = 0; for (; j < nb; j++) if (lb[j].binding == b[i].binding) break;
        if (j == nb) { if (nb == 32) continue; lb[nb++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].kind == NVN_SB_IMG ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : (b[i].kind == NVN_SB_TEX || b[i].kind == NVN_SB_TEX_UBO) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : (b[i].kind == NVN_SB_SBUF || b[i].kind == NVN_SB_SBUF_UBO) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; }
    }
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = (uint32_t)nb, .pBindings = lb};
    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe;
    if (vkCreateDescriptorSetLayout(c->dev, &di, NULL, &dsl)) return NULL;
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
    if (vkCreatePipelineLayout(c->dev, &li, NULL, &pl)) return NULL;
    VkShaderModule mod = nvn_shader_module((nvn_shader *)sh); if (!mod) return NULL;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, 32};
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, (&dm_switch2_gfx && dm_switch2_gfx && c->has_sg32) ? &sg : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}, .layout = pl};       // a Switch 2 compute program runs 32 threads to a warp
    if (vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe)) return NULL;
    g_cp[g_ncp] = (struct cpipe){sh, pipe, pl};
    return &g_cp[g_ncp++];
}
static void trace_note_sampled(uint64_t s);
static void dispatch_impl(const p_dispatch *d) {
    static int f_ind, f_noprog, f_nosh, f_pipe, f_tex, f_cs;
    state_init();
    vkctx_t *c = nvn_vk_ctx(); if (!c) return;
    static _Atomic unsigned ndisp; unsigned k = atomic_fetch_add(&ndisp, 1) + 1;
    VkBuffer ind_buf = VK_NULL_HANDLE; VkDeviceSize ind_off = 0;
    if (d->ind && (!nvn_vk_pool_buffer(d->ind, 12, &ind_buf, &ind_off) || (ind_off & 3))) { skip("an indirect dispatch whose argument buffer is not in a memory pool", &f_ind); return; }       // the group counts are three u32 the GPU reads from there
    if (!S.cprog) { skip("a dispatch with no compute program bound", &f_noprog); return; }
    uint8_t keys[6][8]; int nk = nvn_prog_keys(S.cprog, keys);
    g_cs_units = g_cs_rows = 0;
    { static const char *kp = (const char *)-1; if (kp == (const char *)-1) kp = getenv("NX_CS_KEEP_PUSH");       // NX_CS_KEEP_PUSH=<key prefixes>: those programs, when nothing was pushed for them, get the last block pushed for another (the chain 68b1b94c, 3ba39530, bc48ef31, f3508715 reads the one block the game pushed for the first)
      char kh[20]; snprintf(kh, sizeof kh, "%02x%02x%02x%02x", keys[0][0], keys[0][1], keys[0][2], keys[0][3]);
      if (kp && !S.npushc && g_last_npush && strstr(kp, kh)) { memcpy(S.pushc, g_last_push, g_last_npush); S.npushc = g_last_npush; } }
    if (&dm_switch2_gfx && dm_switch2_gfx && S.cprog->nstage && S.cprog->sd[0].control) {       // the Ounce control section: the dword at +0xb0 holds the push block size (byte 1) and the row count (byte 2)
        uint32_t mg = 0, w = 0; memcpy(&mg, S.cprog->sd[0].control, 4); memcpy(&w, (const uint8_t *)S.cprog->sd[0].control + 0xb0, 4);
        if (mg == 0x98761234u) { g_cs_units = (w >> 8) & 0xff; g_cs_rows = (w >> 16) & 0xff; }
    }
    const nvn_shader *sh = nk ? nvn_shader_find(keys[0]) : NULL;
    if (!sh) { uint8_t ak[8]; if (prog_alt_key(S.cprog, 0, ak) && (sh = nvn_shader_find(ak)) && nk) nvn_shader_alias(keys[0], sh); }
    if (!sh) {
        static int lg = -1; static uint64_t seen[64]; static unsigned nseen; if (lg < 0) lg = getenv("NX_SKIP_LOG") != NULL;
        if (lg && nk) { uint64_t kk; memcpy(&kk, keys[0], 8); unsigned j = 0; while (j < nseen && seen[j] != kk) j++;      // NX_SKIP_LOG: each key a dispatch looked for and the library lacked
            if (j == nseen && nseen < 64) { seen[nseen++] = kk; { extern void nvn_describe_stage(const prog_t *, unsigned); nvn_describe_stage(S.cprog, 0); } { fprintf(stderr, "[dm] [nvn]   compute state for %016llx: tex", (unsigned long long)__builtin_bswap64(kk)); for (int q = 0; q < 8; q++) fprintf(stderr, " %#x", S.tex[5][q]); fprintf(stderr, " img"); for (int q = 0; q < 4; q++) fprintf(stderr, " %#x", S.img[5][q]); fprintf(stderr, " ssbo"); for (int q = 0; q < 6; q++) fprintf(stderr, " %llx/%llu", (unsigned long long)S.ssbo[5][q].addr, (unsigned long long)S.ssbo[5][q].size); fprintf(stderr, " ubo"); for (int q = 0; q < 4; q++) fprintf(stderr, " %llx/%llu", (unsigned long long)S.ubo[5][q].addr, (unsigned long long)S.ubo[5][q].size); fprintf(stderr, "\n"); } fprintf(stderr, "[dm] [nvn] dispatch looks for compute key %016llx (%d keys), %d x %d x %d groups: not in the library\n", (unsigned long long)__builtin_bswap64(kk), nk, d->x, d->y, d->z); } }
        else if (lg && !nk) { static int once; if (!once++) fprintf(stderr, "[dm] [nvn] a dispatch's compute program has no key\n"); }
        { extern int nvn_trace_frame_now(void); if (nvn_trace_frame_now() && nk) { uint64_t kk; memcpy(&kk, keys[0], 8); fprintf(stderr, "[dm] [nvn] trace skip dispatch: compute key %016llx not in the library, %u x %u x %u groups\n", (unsigned long long)__builtin_bswap64(kk), d->x, d->y, d->z); } }
        if (nk && getenv("NX_SHADER_DUMP")) { extern void nvn_dump_stage_key(const prog_t *, unsigned, const uint8_t *); nvn_dump_stage_key(S.cprog, 0, keys[0]); }
        skip("the compute program is not in the shader library", &f_nosh); return;
    }
    if (nvn_shader_stage(sh) != NVN_STAGE_COMPUTE) { skip("the program bound for compute is not a compute shader", &f_cs); return; }
    if (nvn_shader_needs(sh)) { static int f_needs; char why[120]; snprintf(why, sizeof why, "a compute shader needs what the dispatch path cannot supply yet (needs %#x, key %02x%02x%02x%02x, %d x %d x %d groups)", nvn_shader_needs(sh), keys[0][0], keys[0][1], keys[0][2], keys[0][3], d->x, d->y, d->z); skip(why, &f_needs); return; }
    { static const char *only = (const char *)-1; static const char *skipk = (const char *)-1; if (only == (const char *)-1) { only = getenv("NX_CS_ONLY"); skipk = getenv("NX_CS_SKIP"); }       // NX_CS_ONLY=<key prefixes, comma separated>: dispatch only those programs; NX_CS_SKIP: all but those (to find which hangs the GPU)
      char kh[20]; snprintf(kh, sizeof kh, "%02x%02x%02x%02x", keys[0][0], keys[0][1], keys[0][2], keys[0][3]);
      if (only && !strstr(only, kh)) return; if (skipk && strstr(skipk, kh)) return; }
    g_disp_dim[0] = d->x; g_disp_dim[1] = d->y; g_disp_dim[2] = d->z;
    { extern void nvn_cs_note(const uint8_t *); nvn_cs_note(keys[0]); }       // NX_SUBMIT_SYNC=1: the keys dispatched in a command buffer, printed if its submit fails
    VkCommandBuffer cb = nvn_vk_cmd();                                   // ends the render pass: a dispatch cannot run inside one
    if (!cb) return;
    VkDescriptorImageInfo ti[16]; uint32_t tbind[16]; int nt = 0;
    int n; const nvn_shbind *b = nvn_shader_binds(sh, &n);
    for (int i = 0; i < n; i++) {
        if (b[i].kind != NVN_SB_TEX && b[i].kind != NVN_SB_TEX_UBO) continue;
        if ((b[i].kind == NVN_SB_TEX && b[i].reg >= NTEXSLOT) || nt == 16) { skip("a compute shader samples more textures than the dispatch path binds", &f_tex); return; }
        int r = nvn_vk_desc_sampled(bind_handle(5, &b[i]), (int)b[i].aux, &ti[nt]);
        { static int bl = -1; static unsigned nb; if (bl < 0) bl = getenv("NX_CS_BINDLOG") != NULL; if (bl && (nb++ % 300) < 2) { extern uint64_t nvn_vk_desc_serial(uint32_t); extern const uint8_t *nvn_shader_keyof(const nvn_shader *); uint32_t hh = bind_handle(5, &b[i]); fprintf(stderr, "[dm] [nvn] compute sampled bind: shader %02x%02x binding %u handle %#x serial %llu result %d view %p\n", g_dbg_keys[0][0], g_dbg_keys[0][1], b[i].binding, hh, (unsigned long long)nvn_vk_desc_serial(hh), r, (void *)ti[nt].imageView); } }
        if (r != 1) {
            static int lg = -1; static uint32_t seenk[32]; static unsigned nk2; if (lg < 0) lg = getenv("NX_SKIP_LOG") != NULL;       // NX_SKIP_LOG: which program, texture handle and result (3 = sampler type differs) stopped a dispatch, once per program
            uint32_t k32; memcpy(&k32, keys[0], 4); unsigned j = 0; while (j < nk2 && seenk[j] != k32) j++;
            if (lg && j == nk2 && nk2 < 32) { seenk[nk2++] = k32; fprintf(stderr, "[dm] [nvn] compute %02x%02x%02x%02x cannot sample: binding %u reg %u handle %#x want %#x result %d\n", keys[0][0], keys[0][1], keys[0][2], keys[0][3], b[i].binding, b[i].reg, bind_handle(5, &b[i]), (unsigned)b[i].aux, r); }
            { extern int nvn_trace_frame_now(void); if (nvn_trace_frame_now()) fprintf(stderr, "[dm] [nvn] trace skip dispatch: compute key %02x%02x%02x%02x cannot sample handle %#x\n", keys[0][0], keys[0][1], keys[0][2], keys[0][3], bind_handle(5, &b[i])); }
            { const char *rk = getenv("NX_CS_ROWS"); char kh[20]; snprintf(kh, sizeof kh, "%02x%02x%02x%02x", keys[0][0], keys[0][1], keys[0][2], keys[0][3]); static unsigned nsk;
              if (rk && strstr(rk, kh) && nsk++ < 8) { fprintf(stderr, "[dm] [nvn] skipped %s: binding %u reg %u handle %#x want %#x result %d; push (c0+0x10, %u bytes):", kh, b[i].binding, b[i].reg, bind_handle(5, &b[i]), (unsigned)b[i].aux, r, (unsigned)S.npushc); for (unsigned q = 0; q + 4 <= S.npushc && q < 0x100; q += 4) { uint32_t v; memcpy(&v, S.pushc + q, 4); fprintf(stderr, " %x", v); } fprintf(stderr, " | tex"); for (int q = 0; q < NTEXSLOT; q++) fprintf(stderr, " %#x", S.tex[5][q]); fprintf(stderr, " img"); for (int q = 0; q < 4; q++) fprintf(stderr, " %#x", S.img[5][q]);
        { extern uint64_t nvn_vk_desc_serial(uint32_t); extern int nvn_vk_debug_info(uint64_t, unsigned *, unsigned *, unsigned *, unsigned *, unsigned *); fprintf(stderr, " | desc");
          for (int q = 0; q < 8; q++) if (S.tex[5][q] && S.tex[5][q] != 0x10200102) { unsigned f = 0, w = 0, h = 0, l = 0, dd = 0; uint64_t sr = nvn_vk_desc_serial(S.tex[5][q]); nvn_vk_debug_info(sr, &f, &w, &h, &l, &dd); fprintf(stderr, " t%d:%llu/f%#x/%ux%ux%u", q, (unsigned long long)sr, f, w, h, dd); }
          for (int q = 0; q < 4; q++) if (S.img[5][q]) { unsigned f = 0, w = 0, h = 0, l = 0, dd = 0; uint64_t sr = nvn_vk_desc_serial(S.img[5][q]); nvn_vk_debug_info(sr, &f, &w, &h, &l, &dd); fprintf(stderr, " i%d:%llu/f%#x/%ux%ux%u", q, (unsigned long long)sr, f, w, h, dd); }
          { extern int nvn_trace_frame_now(void); if (nvn_trace_frame_now()) { for (int q = 0; q < 8; q++) if (S.tex[5][q] && S.tex[5][q] != 0x10200102) trace_note_prio(nvn_vk_desc_serial(S.tex[5][q])); for (int q = 0; q < 4; q++) if (S.img[5][q]) trace_note_prio(nvn_vk_desc_serial(S.img[5][q])); } } }
        fprintf(stderr, "\n"); } }
            skip("a compute shader reads a texture that cannot be sampled", &f_tex); return; }
        tbind[nt++] = b[i].binding;
    }
    VkDescriptorImageInfo ci[8]; uint32_t cibind[8]; int nci = 0;
    for (int i = 0; i < n; i++) {
        if (b[i].kind != NVN_SB_IMG) continue;
        VkFormat ifmt;
        if (b[i].reg >= 8 || nci == 8 || !nvn_vk_desc_storage(S.img[5][b[i].reg], &ci[nci], &ifmt)) { char why[160]; snprintf(why, sizeof why, "compute image slot %u (handle %#x) has no storage image the shader can use", b[i].reg, S.img[5][b[i].reg & 7]); skip(why, &f_tex); return; }
        cibind[nci++] = b[i].binding;
    }
    if (nvn_trace_frame_now()) { extern uint64_t nvn_vk_desc_serial(uint32_t); extern void nvn_draw_trace_note_target(uint64_t);       // the traced frame dumps what its dispatches read and write
        for (int i = 0; i < n; i++) { uint32_t h = b[i].kind == NVN_SB_IMG && b[i].reg < 8 ? S.img[5][b[i].reg] : b[i].kind == NVN_SB_TEX && b[i].reg < NTEXSLOT ? S.tex[5][b[i].reg] : 0; uint64_t sr = h ? nvn_vk_desc_serial(h) : 0; if (sr) { nvn_draw_trace_note_target(sr); trace_note_sampled(sr); } } }
    struct cpipe *cp = compute_pipeline(c, sh);
    if (!cp) { skip("the compute pipeline could not be built", &f_pipe); return; }
    VkDescriptorBufferInfo bi[32]; VkWriteDescriptorSet wr[32]; int nw = 0;
    for (int i = 0; i < n && nw < 32; i++) {
        if (b[i].kind == NVN_SB_SUPPORT || b[i].kind == NVN_SB_DRV0 || b[i].kind == NVN_SB_DRV2) { bi[nw] = driver_buffer(c, (int)b[i].kind, 5, 0, 0); wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nw]}; nw++; continue; }
        if (b[i].kind == NVN_SB_C1) { bi[nw] = nvn_shader_c1(sh); if (!bi[nw].buffer) bi[nw] = (VkDescriptorBufferInfo){zero_buffer(c), 0, 4096}; wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nw]}; nw++; continue; }
        if (b[i].kind == NVN_SB_SBUF && b[i].reg < 16) {
            const p_bind_buf *u = &S.ssbo[5][b[i].reg]; VkBuffer sbuf; VkDeviceSize soff;
            if (u->addr && u->size && nvn_vk_pool_buffer(u->addr, u->size, &sbuf, &soff)) bi[nw] = (VkDescriptorBufferInfo){sbuf, soff, u->size}; else bi[nw] = (VkDescriptorBufferInfo){zero_storage(c), 0, 4096};
            wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[nw]}; nw++; continue; }
        if (b[i].kind == NVN_SB_SBUF_UBO && b[i].reg < 16) {                   // the game's uniform slot, bound as the storage buffer the shader reads it through
            const p_bind_buf *u = &S.ubo[5][b[i].reg]; VkBuffer sbuf; VkDeviceSize soff;
            if (u->addr && u->size && nvn_vk_pool_buffer(u->addr, u->size, &sbuf, &soff)) bi[nw] = (VkDescriptorBufferInfo){sbuf, soff, u->size}; else bi[nw] = (VkDescriptorBufferInfo){zero_storage(c), 0, 4096};
            wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[nw]}; nw++; continue; }
        if (b[i].kind != NVN_SB_CBUF || b[i].reg >= 16) continue;
        const p_bind_buf *u = &S.ubo[5][b[i].reg]; VkBuffer ub; VkDeviceSize uoff;
        if (u->addr && u->size && nvn_vk_pool_buffer(u->addr, u->size, &ub, &uoff)) bi[nw] = (VkDescriptorBufferInfo){ub, uoff, u->size}; else bi[nw] = (VkDescriptorBufferInfo){zero_buffer(c), 0, 4096};
        wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b[i].binding, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nw]}; nw++;
    }
    for (int i = 0; i < nt && nw < 32; i++) wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = tbind[i], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ti[i]};
    for (int i = 0; i < nci && nw < 32; i++) wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = cibind[i], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ci[i]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    if (nw) c->CmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->layout, 0, (uint32_t)nw, wr);
    { static int mg = -2; if (mg == -2) mg = getenv("NX_CS_MAXGROUPS") ? atoi(getenv("NX_CS_MAXGROUPS")) : -1;       // NX_CS_MAXGROUPS=n: no dispatch runs more than n groups along x (to tell a slow shader from a hung one)
      if (mg > 0 && d->x > mg) { vkCmdDispatch(cb, (uint32_t)mg, d->y > 0 ? (uint32_t)d->y : 0, d->z > 0 ? (uint32_t)d->z : 0); goto dispatched; } }
    if (ind_buf) { vkCmdDispatchIndirect(cb, ind_buf, ind_off); goto dispatched; }
    vkCmdDispatch(cb, d->x > 0 ? (uint32_t)d->x : 0, d->y > 0 ? (uint32_t)d->y : 0, d->z > 0 ? (uint32_t)d->z : 0);
    dispatched:
    nvn_vk_barrier_all(cb);
    { static const char *rk = (const char *)-1; if (rk == (const char *)-1) rk = getenv("NX_CS_READ");       // NX_CS_READ=<key prefixes> (see nvn_vk_tex_read_record): what the images bound to those programs hold right after each of their dispatches in a traced frame
      extern int nvn_trace_frame_now(void); char kh[20]; snprintf(kh, sizeof kh, "%02x%02x%02x%02x", keys[0][0], keys[0][1], keys[0][2], keys[0][3]);
      if (rk && nvn_trace_frame_now() && strstr(rk, kh)) { extern uint64_t nvn_vk_desc_serial(uint32_t); extern int nvn_vk_tex_read_record(VkCommandBuffer, uint64_t, const char *); static unsigned nd; unsigned dn = nd++; uint64_t done[16]; int nn = 0;
        for (int q = 0; q < 12; q++) { uint32_t h = q < 8 ? S.tex[5][q] : S.img[5][q - 8]; if (!h || h == 0x10200102) continue; uint64_t sr = nvn_vk_desc_serial(h); int dup = 0; for (int j = 0; j < nn; j++) if (done[j] == sr) dup = 1; if (dup || nn >= 16) continue; done[nn++] = sr;
          char tag[96]; snprintf(tag, sizeof tag, "d%u_%s_%c%d_%llu", dn, kh, q < 8 ? 't' : 'i', q < 8 ? q : q - 8, (unsigned long long)sr); nvn_vk_tex_read_record(cb, sr, tag); } } }
    { static int csf = -2; if (csf == -2) { const char *e = getenv("NX_CS_SYNC"); csf = e ? atoi(e) : -1; }          // NX_CS_SYNC=<present>: from that frame on every dispatch is printed (key, groups, storage and uniform rows) and waited for, so the one that faults is the last line
      extern uint64_t nvn_present_count(void); extern void mtl_sub_cut(void); extern uint64_t mtl_last_fence(void); extern int mtl_wait(uint64_t, uint64_t);
      if (csf >= 0 && (int)nvn_present_count() >= csf) {
        fprintf(stderr, "[dm] [nvn] syncdisp present %d key %02x%02x%02x%02x groups %d,%d,%d ind %llx | ", (int)nvn_present_count(), keys[0][0], keys[0][1], keys[0][2], keys[0][3], d->x, d->y, d->z, (unsigned long long)d->ind);
        for (int r = 0; r < 6; r++) if (S.ssbo[5][r].addr) fprintf(stderr, "ssbo%d %#llx+%#x ", r, (unsigned long long)S.ssbo[5][r].addr, (unsigned)S.ssbo[5][r].size);
        for (int r = 0; r < 4; r++) if (S.ubo[5][r].addr) fprintf(stderr, "ubo%d %#llx+%#x ", r, (unsigned long long)S.ubo[5][r].addr, (unsigned)S.ubo[5][r].size);
        if (S.ubo[5][0].addr && S.ubo[5][0].size >= 0x84) { extern int nvn_vk_pool_describe(uint64_t, uint64_t[6]); uint64_t dd[6]; if (nvn_vk_pool_describe(S.ubo[5][0].addr, dd)) { uint32_t w[4]; memcpy(w, (const void *)(uintptr_t)(S.ubo[5][0].addr + 0x7c), 16); fprintf(stderr, "| ubo0[0x7c..0x88] %#x %#x %#x %#x ", w[0], w[1], w[2], w[3]); } }
        fprintf(stderr, "| push %u:", S.npushc);
        for (unsigned q = 0; q + 8 <= S.npushc && q < 0x40; q += 8) { uint64_t v; memcpy(&v, S.pushc + q, 8); extern int nvn_vk_pool_describe(uint64_t, uint64_t[6]); uint64_t dd[6]; fprintf(stderr, " %#llx%s", (unsigned long long)v, v > 0x1000000 && nvn_vk_pool_describe(v, dd) ? "(pool)" : ""); }
        if (getenv("NX_CS_SYNC_DATA")) {                // NX_CS_SYNC_DATA=1 (with NX_CS_SYNC): the first 8 words of every storage row and of the indirect arguments, as the GPU will read them
          extern int nvn_vk_pool_describe(uint64_t, uint64_t[6]); uint64_t dd[6];
          if (keys[0][0] == 0x24 && keys[0][1] == 0xff && S.ssbo[5][1].addr && nvn_vk_pool_describe(S.ssbo[5][1].addr, dd)) {      // 24ff13bd: element i of ssbo1 is 16 bytes, its word 3 indexes ssbo4 in 16 byte steps
            uint32_t mx = 0, mn = ~0u; unsigned bad = 0, nz = 0, ne = (unsigned)(S.ssbo[5][1].size / 16); const uint32_t *w = (const uint32_t *)(uintptr_t)S.ssbo[5][1].addr;
            for (unsigned i = 0; i < ne; i++) { uint32_t v = w[i * 4 + 3]; if (v > mx) mx = v; if (v < mn) mn = v; if (v > S.ssbo[5][4].size / 16) bad++; if (v) nz++; }
            fprintf(stderr, "\n[dm] [nvn]   ssbo1 index word: %u elements, min %u max %u nonzero %u beyond ssbo4 %u (ssbo4 holds %u)", ne, mn, mx, nz, bad, (unsigned)(S.ssbo[5][4].size / 16)); }
          fprintf(stderr, "\n[dm] [nvn]   data:");
          if (d->ind && nvn_vk_pool_describe(d->ind, dd)) { uint32_t g[3]; memcpy(g, (const void *)(uintptr_t)d->ind, 12); fprintf(stderr, " ind=%u,%u,%u", g[0], g[1], g[2]); }
          for (int r = 0; r < 8; r++) if (S.ubo[5][r].addr && nvn_vk_pool_describe(S.ubo[5][r].addr, dd)) { uint64_t q[16] = {0}; size_t n = S.ubo[5][r].size < 128 ? S.ubo[5][r].size : 128; memcpy(q, (const void *)(uintptr_t)S.ubo[5][r].addr, n); fprintf(stderr, " | u%d(%#x):", r, (unsigned)S.ubo[5][r].size); for (int i = 0; i < 16; i++) fprintf(stderr, " %llx", (unsigned long long)q[i]); }
          for (int r = 0; r < 8; r++) if (S.ubo[5][r].addr && S.ubo[5][r].size >= 0x100 && S.ubo[5][r].size <= 0x200 && nvn_vk_pool_describe(S.ubo[5][r].addr, dd)) {      // a pointer table: each device address in it and the 8 bytes it points at
            extern uint64_t nvn_gpu_addr_in(uint64_t); fprintf(stderr, "\n[dm] [nvn]   table u%d:", r);
            for (unsigned o = 0; o + 16 <= S.ubo[5][r].size; o += 16) { uint64_t v, w = 0; memcpy(&v, (const void *)(uintptr_t)(S.ubo[5][r].addr + o), 8); uint64_t h = nvn_gpu_addr_in(v); int ok = v >= 0xffff800000000000ull && h != v && nvn_vk_pool_describe(h, dd); if (ok) memcpy(&w, (const void *)(uintptr_t)h, 8); fprintf(stderr, " [%x]%llx%s", o, (unsigned long long)v, ok ? "" : "!"); if (ok) fprintf(stderr, "->%llx", (unsigned long long)w); }
          }
          for (int r = 0; r < 8; r++) if (S.ubo[5][r].addr && S.ubo[5][r].size >= 0x700 && nvn_vk_pool_describe(S.ubo[5][r].addr, dd)) { uint32_t q[64]; memcpy(q, (const void *)(uintptr_t)(S.ubo[5][r].addr + 0x640), sizeof q); fprintf(stderr, "\n[dm] [nvn]   u%d[0x640..0x740]:", r); for (int i = 0; i < 64; i++) fprintf(stderr, " %x", q[i]); }
          extern uint64_t nvn_gpu_addr_in(uint64_t); for (int r = 0; r < 6; r++) if (S.ssbo[5][r].addr && nvn_vk_pool_describe(S.ssbo[5][r].addr, dd)) { uint64_t q[8] = {0}; size_t n = S.ssbo[5][r].size < 64 ? S.ssbo[5][r].size : 64; memcpy(q, (const void *)(uintptr_t)S.ssbo[5][r].addr, n); fprintf(stderr, " | s%d [pool %llx+%llx dev %llx%s]:", r, (unsigned long long)dd[0], (unsigned long long)dd[1], (unsigned long long)dd[4], S.ssbo[5][r].addr + S.ssbo[5][r].size > dd[2] + dd[3] ? " PAST ITS END" : ""); for (int i = 0; i < 8; i++) { uint64_t h = nvn_gpu_addr_in(q[i]), z[6]; fprintf(stderr, " %llx", (unsigned long long)q[i]); if (q[i] >= 0xffff800000000000ull) { if (h != q[i] && nvn_vk_pool_describe(h, z)) fprintf(stderr, "(pool %llx+%llx)", (unsigned long long)z[0], (unsigned long long)z[1]); else fprintf(stderr, "(NO POOL)"); } } }
        }
        fprintf(stderr, "\n"); fflush(stderr);
        mtl_sub_cut(); mtl_wait(mtl_last_fence(), 20ull * 1000000000ull);
      } }                                              // what the dispatch wrote is visible to every later read, draw, copy or dispatch
    if (nvn_trace_frame_now()) fprintf(stderr, "[dm] [nvn] trace dispatch: shader %02x%02x%02x%02x, %d x %d x %d groups, %d bindings, g-ssbo5: %llx/%llu %llx/%llu %llx/%llu\n", keys[0][0], keys[0][1], keys[0][2], keys[0][3], d->x, d->y, d->z, nw, (unsigned long long)S.ssbo[5][0].addr, (unsigned long long)S.ssbo[5][0].size, (unsigned long long)S.ssbo[5][1].addr, (unsigned long long)S.ssbo[5][1].size, (unsigned long long)S.ssbo[5][2].addr, (unsigned long long)S.ssbo[5][2].size);
    { static const char *rk = (const char *)-1; if (rk == (const char *)-1) rk = getenv("NX_CS_ROWS");       // NX_CS_ROWS=<key prefix>: print the storage rows, the push block and the group counts of each dispatch of that program (for a program that hangs the GPU)
      char kh[20]; snprintf(kh, sizeof kh, "%02x%02x%02x%02x", keys[0][0], keys[0][1], keys[0][2], keys[0][3]);
      if (rk && strstr(rk, kh)) { fprintf(stderr, "[dm] [nvn] rows of %s: units %u rows %u npushc %u groups %u x %u x %u ind %#llx:", kh, g_cs_units, g_cs_rows, (unsigned)S.npushc, d->x, d->y, d->z, (unsigned long long)d->ind); for (int r = 0; r < 10; r++) fprintf(stderr, " %d=%llx/%u", r, (unsigned long long)S.ssbo[5][r].addr, (unsigned)S.ssbo[5][r].size); { static unsigned nw; if (nw++ < 4) for (int r = 0; r < 10; r++) if (S.ssbo[5][r].addr && S.ssbo[5][r].size) { uint32_t w8[8] = {0}; struct iovec lv = {w8, sizeof w8}, rv = {(void *)(uintptr_t)S.ssbo[5][r].addr, sizeof w8}; ssize_t got = process_vm_readv(getpid(), &lv, 1, &rv, 1, 0); fprintf(stderr, "[dm] [nvn]   row %d first words (%zd bytes read):", r, got); for (int q = 0; q < 8; q++) fprintf(stderr, " %08x", w8[q]); fprintf(stderr, "\n"); } }
      fprintf(stderr, " | push (c0+0x10):"); for (unsigned q = 0; q + 4 <= S.npushc && q < 0x100; q += 4) { uint32_t v; memcpy(&v, S.pushc + q, 4); fprintf(stderr, " %x", v); } fprintf(stderr, " | tex"); for (int q = 0; q < NTEXSLOT; q++) fprintf(stderr, " %#x", S.tex[5][q]); fprintf(stderr, " img"); for (int q = 0; q < 4; q++) fprintf(stderr, " %#x", S.img[5][q]);
        { extern uint64_t nvn_vk_desc_serial(uint32_t); extern int nvn_vk_debug_info(uint64_t, unsigned *, unsigned *, unsigned *, unsigned *, unsigned *); fprintf(stderr, " | desc");
          for (int q = 0; q < 8; q++) if (S.tex[5][q] && S.tex[5][q] != 0x10200102) { unsigned f = 0, w = 0, h = 0, l = 0, dd = 0; uint64_t sr = nvn_vk_desc_serial(S.tex[5][q]); nvn_vk_debug_info(sr, &f, &w, &h, &l, &dd); fprintf(stderr, " t%d:%llu/f%#x/%ux%ux%u", q, (unsigned long long)sr, f, w, h, dd); }
          for (int q = 0; q < 4; q++) if (S.img[5][q]) { unsigned f = 0, w = 0, h = 0, l = 0, dd = 0; uint64_t sr = nvn_vk_desc_serial(S.img[5][q]); nvn_vk_debug_info(sr, &f, &w, &h, &l, &dd); fprintf(stderr, " i%d:%llu/f%#x/%ux%ux%u", q, (unsigned long long)sr, f, w, h, dd); } }
        fprintf(stderr, "\n"); } }
    if (k == 1 || (k < 20 && getenv("NX_SKIP_LOG"))) { extern uint64_t nvn_present_count(void); fprintf(stderr, "[dm] [nvn] dispatch #%u (frame %llu): compute shader key %02x%02x%02x%02x, %d x %d x %d groups, %d bindings\n", k, (unsigned long long)nvn_present_count(), sh ? keys[0][0] : 0, keys[0][1], keys[0][2], keys[0][3], d->x, d->y, d->z, nw); }
}

void nvn_dispatch(const p_dispatch *d) {
    dispatch_impl(d);
    if (S.npushc) { memcpy(g_last_push, S.pushc, S.npushc); g_last_npush = S.npushc; }       // see NX_CS_KEEP_PUSH in dispatch_impl
    S.npushc = 0;                                          // the constants pushed for a dispatch are its own: the next program starts with none
}

// NX_DRAW_LOG=1: the first draws made, then one line per 4000, with the totals and the render target they went to
static void draw_note(uint32_t count) {
    g_since_cut++;
    uint64_t n = atomic_fetch_add(&g_ndraw, 1) + 1;
    static int dd = -1; if (dd < 0) dd = getenv("NX_DRAW_DETAIL") != NULL;
    if (dd && n >= 6000 && n < 6024) { const blend_st *bl = &S.blend[0]; fprintf(stderr, "[dm] [nvn] draw %llu: target %llu chmask %#x blend_en %#x src_rgb %u dst_rgb %u eq %u alpha_func %u keys %02x%02x/%02x%02x\n", (unsigned long long)n, (unsigned long long)g_rt.cser[0], S.chmask.mask & 0xf, S.color.blend_en, (unsigned)bl->src_rgb, (unsigned)bl->dst_rgb, (unsigned)bl->eq_rgb, S.color.alpha_func, S.keys[0][0], S.keys[0][1], S.keys[1][0], S.keys[1][1]); }
    { static int sm = -1; static unsigned ns; if (sm < 0) sm = getenv("NX_SMALL_LOG") != NULL;       // NX_SMALL_LOG=1: the first 80 draws into an offscreen target narrower than 800 pixels (text, icons): target, size, count, keys, the texture handle of each bound slot that is not the placeholder
      if (sm && g_rt.n && g_rt.cser[0] > 100 && P.w < 800 && ns < 80) { ns++; extern uint64_t nvn_present_count(void); if (ns == 1 && getenv("NX_SMALL_TRACE")) { extern long nvn_trace_request(void), nvn_trace_request_now(void); if (atoi(getenv("NX_SMALL_TRACE")) == 2) (void)nvn_trace_request_now(); else (void)nvn_trace_request(); }       // NX_SMALL_TRACE=1: the first of them also asks for the detailed trace of the frame two on (with NX_FRAME_DUMP: the textures it samples)
        fprintf(stderr, "[dm] [nvn] small draw %u (frame %llu): target %llu %ux%u count %u keys", ns, (unsigned long long)nvn_present_count(), (unsigned long long)g_rt.cser[0], P.w, P.h, count); for (int i = 0; i < S.nkeys; i++) { fprintf(stderr, " "); for (int b = 0; b < 4; b++) fprintf(stderr, "%02x", S.keys[i][b]); }
        fprintf(stderr, " [made with %02x%02x%02x%02x + %02x%02x%02x%02x; s1.8=%#x s1.9=%#x s1.11=%#x]", g_dbg_keys[0][0], g_dbg_keys[0][1], g_dbg_keys[0][2], g_dbg_keys[0][3], g_dbg_keys[1][0], g_dbg_keys[1][1], g_dbg_keys[1][2], g_dbg_keys[1][3], S.tex[1][8], S.tex[1][9], S.tex[1][11]); fprintf(stderr, " tex"); for (int st = 0; st < 2; st++) for (int sl = 0; sl < NTEXSLOT; sl++) if (S.tex[st][sl] && S.tex[st][sl] != 0x10200102) { extern uint64_t nvn_vk_desc_serial(uint32_t); extern int nvn_vk_debug_info(uint64_t, unsigned *, unsigned *, unsigned *, unsigned *, unsigned *); uint64_t sr = nvn_vk_desc_serial(S.tex[st][sl]); unsigned tf = 0, tw = 0, th = 0, tl = 0, td = 0; int have = sr ? nvn_vk_debug_info(sr, &tf, &tw, &th, &tl, &td) : 0; fprintf(stderr, " s%d.%d=%#x(ser%llu%s%#x %ux%u)", st, sl, S.tex[st][sl], (unsigned long long)sr, have ? " fmt " : " none ", tf, tw, th); if (ns == 1 && sr && getenv("NX_FRAME_DUMP")) { extern long nvn_vk_debug_lit(uint64_t, size_t *, const char *); char fn[600]; size_t tot = 0; snprintf(fn, sizeof fn, "%s.small%llu.ppm", getenv("NX_FRAME_DUMP"), (unsigned long long)sr); long lit = nvn_vk_debug_lit(sr, &tot, fn); fprintf(stderr, "[lit %llu: %ld/%zu]", (unsigned long long)sr, lit, tot); } fprintf(stderr, " blend_en %#x\n", S.color.blend_en); } } }
    if (g_rt.n && g_rt.cser[0] > 100) atomic_store(&g_main_target, g_rt.cser[0]);       // the last offscreen colour target (the window textures have small serials)
    { static int per = -1; if (per < 0) per = getenv("NX_DRAW_LOG") != NULL;        // draws per colour target serial, listed every 4000 draws
      if (per) { static uint64_t ser[16]; static uint64_t cnt[16]; static int ns; int k = 0; for (; k < ns; k++) if (ser[k] == g_rt.cser[0]) break;
        if (k == ns && ns < 16) { ser[ns] = g_rt.cser[0]; cnt[ns++] = 0; } if (k < 16) cnt[k]++;
        if (n % 4000 == 0) { fprintf(stderr, "[dm] [nvn] draws per colour target:"); for (int i = 0; i < ns; i++) fprintf(stderr, " %llu:%llu", (unsigned long long)ser[i], (unsigned long long)cnt[i]); fprintf(stderr, "\n"); } } }
    static int on = -1; if (on < 0) on = getenv("NX_DRAW_LOG") != NULL;
    if (on && (n <= 5 || n % 4000 == 0)) fprintf(stderr, "[dm] [nvn] draw %llu made (%u vertices/indices), %llu skipped so far, render targets %d, depth-stencil %d, pass %ux%u, colour target serial %llu\n", (unsigned long long)n, count, (unsigned long long)atomic_load(&g_nskip), g_rt.n, g_rt.dser ? 1 : 0, P.w, P.h, (unsigned long long)g_rt.cser[0]);
}

// NX_DRAW_TRACE=<frame>: one line for each draw of that presented frame (its number counts the presents so far): the colour and depth targets, the depth and cull state, the keys of the program's
// stages and the size. NX_DRAW_SKIP=<a>-<b>: the draws of that frame whose index lies in the range are made ready but not drawn, to see which of them paint a region (compare the two frames).
static uint64_t g_trace_ser[600]; static int g_trace_n;                       // the colour and depth targets drawn into during the traced frame (dumped by the present)
static uint64_t g_samp_ser[400]; static int g_samp_n;      // the textures the traced frame's draws sampled (its first draws up to 400 of them), dumped through the GPU's own decoded view
static void trace_note_sampled(uint64_t s) { for (int i = 0; i < g_samp_n; i++) if (g_samp_ser[i] == s) return; if (g_samp_n < 400) g_samp_ser[g_samp_n++] = s; }
static uint64_t g_samp_prio[64]; static int g_samp_prio_n;      // the textures of the NX_CS_ROWS programs: dumped before the draws' (the list of those fills up first)
static void trace_note_prio(uint64_t s) { for (int i = 0; i < g_samp_prio_n; i++) if (g_samp_prio[i] == s) return; if (g_samp_prio_n < 64) g_samp_prio[g_samp_prio_n++] = s; }
int nvn_draw_trace_sampled(uint64_t *out, int max) { int n = 0; for (int i = 0; i < g_samp_prio_n && n < max; i++) out[n++] = g_samp_prio[i]; for (int i = 0; i < g_samp_n && n < max; i++) { int dup = 0; for (int j = 0; j < g_samp_prio_n; j++) if (g_samp_prio[j] == g_samp_ser[i]) dup = 1; if (!dup) out[n++] = g_samp_ser[i]; } return n; }
void nvn_draw_trace_note_target(uint64_t s) { for (int i = 0; i < g_trace_n; i++) if (g_trace_ser[i] == s) return; if (g_trace_n < 600) g_trace_ser[g_trace_n++] = s; }
int nvn_draw_trace_targets(uint64_t *out, int max) { int n = g_trace_n < max ? g_trace_n : max; memcpy(out, g_trace_ser, (size_t)n * sizeof *out); return n; }
extern uint64_t nvn_present_count(void);
// A frame to trace asked for while the title runs (F12 in the window): the one two frames on, so that it is recorded whole
static _Atomic long g_req_frame = -1; static _Atomic int g_req_n;
int nvn_trace_request_count(void) { return atomic_load(&g_req_n); }
long nvn_trace_request_now(void) { atomic_fetch_add(&g_req_n, 1); long f = (long)nvn_present_count(); atomic_store(&g_req_frame, f); return f; }
long nvn_trace_request(void) { atomic_fetch_add(&g_req_n, 1); long f = (long)nvn_present_count() + 2; atomic_store(&g_req_frame, f); return f; }
static void trace_signal(int sig) { (void)sig; nvn_trace_request(); }      // NX_TRACE_SIGNAL=1: kill -USR1 traces the frame two on, and with NX_FRAME_DUMP dumps its targets, at whatever screen the title is on
long nvn_trace_target(void) { static long frame = -2; if (frame == -2) { const char *e = getenv("NX_DRAW_TRACE"); frame = e ? atol(e) : -1; if (getenv("NX_TRACE_SIGNAL")) signal(SIGUSR1, trace_signal); } return frame >= 0 ? frame : atomic_load(&g_req_frame); }
static long trace_target(void) { return nvn_trace_target(); }
static int trace_detail(void) { static int on = -1; if (on < 0) on = getenv("NX_DRAW_TRACE_DETAIL") != NULL; return on || atomic_load(&g_req_frame) >= 0; }
int nvn_trace_frame_now(void) {        // 1 while the frame NX_DRAW_TRACE (or the F12 request) names is being replayed
    long frame = trace_target(); return frame >= 0 && (long)nvn_present_count() == frame;
}
static int trace_draw(const char *what, uint32_t count, int *skip_it) {
    long frame = trace_target();
    *skip_it = 0;
    if (frame < 0 || (long)nvn_present_count() != frame) return 0;      // first: this runs for every draw, and the getenv below only matters in the traced frame (two getenv per draw were 0.3 % of a 2P race)
    long sa = -1, sb = -1;      // NX_DRAW_SKIP=a-b skips those draws of the traced frame; a list "a-b;c-d;..." gives the k-th F12 / SIGUSR1 request its k-th range ("-" = none)
    { const char *k = getenv("NX_DRAW_SKIP"); int want = getenv("NX_DRAW_TRACE") ? 0 : atomic_load(&g_req_n) - 1; while (k && want-- > 0) { k = strchr(k, ';'); if (k) k++; } if (k) sscanf(k, "%ld-%ld", &sa, &sb); }
    static long idx = -1, seen_frame = -1; if (seen_frame != frame) { seen_frame = frame; idx = -1; }
    idx++;
    if (sa >= 0 && idx >= sa && idx <= sb) *skip_it = 1;
    { const char *kp = getenv("NX_DRAW_KEEP"); if (kp) { int keep = 0; for (const char *q = kp; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL) if (atol(q) == idx) keep = 1; if (!keep) *skip_it = 1; } }       // NX_DRAW_KEEP=<n,m,..>: only those draws of the traced frame are made
    { extern void nvn_draw_trace_note_target(uint64_t); for (int i = 0; i < g_rt.n; i++) if (g_rt.cser[i]) nvn_draw_trace_note_target(g_rt.cser[i]); if (g_rt.dser) nvn_draw_trace_note_target(g_rt.dser); }
    fprintf(stderr, "[dm] [nvn] trace draw %ld %s%s: count %u colour %llu depth %llu rts %d ds dt %d dw %d df %d cull %d front %d blend0 src %u dst %u keys", idx, what, *skip_it ? " (skipped)" : "", count,
            (unsigned long long)g_rt.cser[0], (unsigned long long)g_rt.dser, g_rt.n, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.poly.cull, (int)S.poly.front, S.blend[0].src_rgb, S.blend[0].dst_rgb);
    for (int i = 0; i < S.nkeys; i++) { fprintf(stderr, " "); for (int b = 0; b < 4; b++) fprintf(stderr, "%02x", S.keys[i][b]); }
    fprintf(stderr, "\n");
    if (trace_detail()) {      // channel mask, blend enable, viewport and scissor of the draw
        if (getenv("NX_DRAW_UBO5") && S.ubo[1][5].addr && S.ubo[1][5].size >= 240) { const uint32_t *w = (const uint32_t *)(uintptr_t)S.ubo[1][5].addr; fprintf(stderr, "[dm] [nvn]   draw %ld fragment ubo 5 (%llu bytes): w0-3 %#x %#x %#x %#x w56-60 %#x %#x %#x %#x %#x\n", idx, (unsigned long long)S.ubo[1][5].size, w[0], w[1], w[2], w[3], w[56], w[57], w[58], w[59], w[60]); }
        if (getenv("NX_DESC_RANGE")) { static int done; if (!done++) { int a, b; if (sscanf(getenv("NX_DESC_RANGE"), "%d,%d", &a, &b) == 2) for (int id = a; id <= b; id++) { extern uint64_t nvn_vk_desc_serial(uint32_t); extern int nvn_vk_debug_info(uint64_t, unsigned *, unsigned *, unsigned *, unsigned *, unsigned *); uint64_t sr = nvn_vk_desc_serial((uint32_t)id); unsigned f = 0, w = 0, h = 0, l = 0, d = 0; if (sr) nvn_vk_debug_info(sr, &f, &w, &h, &l, &d); fprintf(stderr, "[dm] [nvn]   texture id %d -> serial %llu fmt %#x %ux%u levels %u\n", id, (unsigned long long)sr, f, w, h, l); } } }       // NX_DESC_RANGE=<first>,<last>: the textures registered under those ids
        fprintf(stderr, "[dm] [nvn]   draw %ld stencil st %d func %d fail %d zfail %d zpass %d | blend0 src_rgb %u dst_rgb %u src_a %u dst_a %u eq_rgb %u eq_a %u | poly cull %d offset %d\n", idx, (int)S.ds.st, (int)S.ds.sf_f, (int)S.ds.fail_f, (int)S.ds.zfail_f, (int)S.ds.zpass_f, (unsigned)S.blend[0].src_rgb, (unsigned)S.blend[0].dst_rgb, (unsigned)S.blend[0].src_a, (unsigned)S.blend[0].dst_a, (unsigned)S.blend[0].eq_rgb, (unsigned)S.blend[0].eq_a, (int)S.poly.cull, (int)S.poly.offset_en);
        fprintf(stderr, "[dm] [nvn]   draw %ld state chmask %#x blend_en %#x logic %u has_vp %d vp0 %g %g %g %g depth %g %g scissor %d: %d %d %d %d\n", idx, (unsigned)S.chmask.mask, (unsigned)S.color.blend_en, (unsigned)S.color.logic_op, S.has_vp, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.vp[0][4], S.vp[0][5], g_sc.has, g_sc.r[0], g_sc.r[1], g_sc.r[2], g_sc.r[3]);
    }
    if (trace_detail()) {      // the vertex attribute formats (NVN format numbers) and the texture handles of each stage
        fprintf(stderr, "[dm] [nvn]   draw %ld attrs", idx);
        for (int i = 0; i < S.nattr && i < 16; i++) { va_st a; memcpy(&a, &S.attr[i], sizeof a); fprintf(stderr, " %d:f%#x/s%u+%u", i, (unsigned)a.fmt, (unsigned)a.stream, (unsigned)a.offset); }
        if (count <= 2000 || count >= 8000) { extern uint64_t nvn_vk_desc_serial(uint32_t); for (int st = 0; st < 6; st++) for (int sl = 0; sl < NTEXSLOT; sl++) if (S.tex[st][sl]) { uint64_t sr = nvn_vk_desc_serial(S.tex[st][sl]); if (sr) nvn_draw_trace_note_target(sr); } }   // the textures a small (lighting) draw reads are dumped too
        fprintf(stderr, " | tex");
        for (int st = 0; st < 6; st++) for (int sl = 0; sl < NTEXSLOT; sl++) if (S.tex[st][sl]) { extern uint64_t nvn_vk_desc_serial(uint32_t); extern int nvn_vk_debug_info(uint64_t, unsigned *, unsigned *, unsigned *, unsigned *, unsigned *);
            uint64_t sr = nvn_vk_desc_serial(S.tex[st][sl]); trace_note_sampled(sr); unsigned tf = 0, tw = 0, th = 0, tl = 0, td = 0; int have = nvn_vk_debug_info(sr, &tf, &tw, &th, &tl, &td);
            fprintf(stderr, " s%d.%d=id%u/smp%u/ser%llu", st, sl, S.tex[st][sl] & 0xfffff, S.tex[st][sl] >> 20, (unsigned long long)sr);
            if (have) fprintf(stderr, "[%#x %ux%u l%u]", tf, tw, th, tl); }
        fprintf(stderr, "\n");
    }
    if (trace_detail() && count <= 64) for (int i = 0; i < S.nstream && i < 16; i++) if (S.vbuf[i].address) {      // the first vertices of a small draw (a quad): where its vertex stream is and what it holds
        const float *f = (const float *)(uintptr_t)S.vbuf[i].address; size_t nf = S.vbuf[i].size / 4 < 24 ? S.vbuf[i].size / 4 : 24;
        fprintf(stderr, "[dm] [nvn]   draw %ld stream %d: addr %#llx size %llu stride %u:", idx, i, (unsigned long long)S.vbuf[i].address, (unsigned long long)S.vbuf[i].size, S.stream[i].stride); for (size_t q = 0; q < nf; q++) fprintf(stderr, " %g", f[q]); fprintf(stderr, "\n"); }
    if (trace_detail() && count <= 64) for (int st = 0; st < 2; st++) if (S.ubo[st][0].addr && S.ubo[st][0].size) {      // the whole uniform block 0 of the vertex and the fragment stage of a small draw (the per-draw constants), as floats
        const float *w = (const float *)(uintptr_t)S.ubo[st][0].addr; size_t nw = S.ubo[st][0].size / 4;
        fprintf(stderr, "[dm] [nvn]   draw %ld block0 of stage %d (%zu words):", idx, st, nw); for (size_t q = 0; q < nw; q++) fprintf(stderr, "%s%g", q % 4 ? " " : "\n[dm] [nvn]     ", w[q]); fprintf(stderr, "\n"); }
    if (count >= 10000 || trace_detail()) for (int st = 0; st < 6; st++) for (int sl = 0; sl < 16; sl++) {      // the uniform buffers a large draw reads: where they are and what they start with
        const p_bind_buf *u = &S.ubo[st][sl]; if (!u->size) continue;
        float f[16] = {0}; size_t n = u->size < sizeof f ? u->size : sizeof f; memcpy(f, (const void *)(uintptr_t)u->addr, n);
        fprintf(stderr, "[dm] [nvn]   draw %ld stage %d ubo %d: addr %#llx size %llu: %g %g %g %g %g %g %g %g | %g %g %g %g %g %g %g %g\n", idx, st, sl, (unsigned long long)u->addr, (unsigned long long)u->size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    }
    if (trace_detail() && getenv("NX_DRAW_SSBO6") && S.ssbo[0][6].addr) { const uint32_t *w = (const uint32_t *)(uintptr_t)S.ssbo[0][6].addr; size_t nw = S.ssbo[0][6].size / 4; fprintf(stderr, "[dm] [nvn]   draw %ld storage buffer 6 of the vertex stage (%zu words):", idx, nw); for (size_t q = 0; q < nw && q < 160; q++) { float f; memcpy(&f, &w[q], 4); fprintf(stderr, "%s%g", q % 8 ? " " : "\n[dm] [nvn]     ", (w[q] & 0x7f800000) == 0x7f800000 || (w[q] && (w[q] & 0x7f800000) == 0) ? (float)0 : f); if ((w[q] & 0x7f800000) == 0 && w[q]) fprintf(stderr, "(i%u)", w[q]); } fprintf(stderr, "\n"); }
    if (trace_detail()) for (int st = 0; st < 2; st++) for (int sl = 0; sl < 16; sl++) {      // the storage buffers a Switch 2 shader reads through the raw pointers of c[0]: slot, address (and its device address), size, first words
        const p_bind_buf *u = &S.ssbo[st][sl]; if (!u->addr) continue;
        float f[8] = {0}; uint32_t w[8] = {0}; size_t n = u->size && u->size < sizeof w ? u->size : sizeof w; memcpy(w, (const void *)(uintptr_t)u->addr, n); memcpy(f, w, sizeof f);
        fprintf(stderr, "[dm] [nvn]   draw %ld stage %d ssbo %d: addr %#llx (device %#llx) size %llu: %g %g %g %g %g %g %g %g | %#x %#x\n", idx, st, sl, (unsigned long long)u->addr, (unsigned long long)nvn_vk_device_address(u->addr, 1), (unsigned long long)u->size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], w[0], w[1]);
    }
    return 1;
}

// NX_VS_SKIP=<vertex key prefixes>: draws with those vertex shaders are dropped (to find which pipeline faults the GPU)
static int vs_skip(void) {
    static const char *want = (const char *)-1; static size_t big; if (want == (const char *)-1) { want = getenv("NX_VS_SKIP"); const char *b = getenv("NX_SKIP_BIG"); big = b ? (size_t)atol(b) : 0; }       // NX_SKIP_BIG=<bytes>: also every draw whose vertex or fragment SPIR-V is at least that big
    if (big && (g_dbg_size[0] >= big || g_dbg_size[1] >= big)) return 1;
    if (!want) return 0;
    char kh[12], kf[12]; snprintf(kh, sizeof kh, "%02x%02x%02x%02x", g_dbg_keys[0][0], g_dbg_keys[0][1], g_dbg_keys[0][2], g_dbg_keys[0][3]);
    snprintf(kf, sizeof kf, "%02x%02x%02x%02x", g_dbg_keys[1][0], g_dbg_keys[1][1], g_dbg_keys[1][2], g_dbg_keys[1][3]); return strstr(want, kh) != NULL || strstr(want, kf) != NULL;       // a prefix of the vertex or the fragment shader
}
extern void mtl_sub_cut(void); extern uint64_t mtl_last_fence(void); extern int mtl_wait(uint64_t, uint64_t);
// NX_DRAW_SYNC_FROM=<present number>: from that present on, each draw is printed, handed to the GPU on its own and waited for, so that the last line printed before a GPU fault names the draw that
// caused it (keys, counts, the storage rows and the first words of the uniform buffers of both stages). Slow: for the last frames before a known fault only.
static void draw_sync_point(const char *kind, uint64_t count, int64_t first, uint32_t inst) {
    static int from = -2; if (from == -2) { const char *e = getenv("NX_DRAW_SYNC_FROM"); from = e ? atoi(e) : -1; }
    if (from < 0) return;
    extern uint64_t nvn_present_count(void); if ((int)nvn_present_count() < from) return;
    static unsigned idx;
    fprintf(stderr, "[dm] [nvn] syncdraw %u present %d %s vs %02x%02x%02x%02x fs %02x%02x%02x%02x count %llu first/base %lld inst %u prim | ", idx++, (int)nvn_present_count(), kind,
            g_dbg_keys[0][0], g_dbg_keys[0][1], g_dbg_keys[0][2], g_dbg_keys[0][3], g_dbg_keys[1][0], g_dbg_keys[1][1], g_dbg_keys[1][2], g_dbg_keys[1][3], (unsigned long long)count, (long long)first, inst);
    for (int st = 0; st < 2; st++) for (int r = 0; r < 3; r++) if (S.ssbo[st][r].addr) fprintf(stderr, "ssbo%d.%d %#llx+%#x ", st, r, (unsigned long long)S.ssbo[st][r].addr, (unsigned)S.ssbo[st][r].size);
    for (int st = 0; st < 2; st++) for (int r = 0; r < 4; r++) if (S.ubo[st][r].addr) { fprintf(stderr, "ubo%d.%d %#llx+%#x:", st, r, (unsigned long long)S.ubo[st][r].addr, (unsigned)S.ubo[st][r].size); const uint32_t *w = (const uint32_t *)(uintptr_t)S.ubo[st][r].addr; for (int q = 0; q < 8 && q * 4 < (int)S.ubo[st][r].size; q++) fprintf(stderr, "%x,", w[q]); fprintf(stderr, " "); }
    fprintf(stderr, "| idx %lld..%lld ", (long long)g_sync_imin, (long long)g_sync_imax);
    { extern int nvn_vk_pool_describe(uint64_t, uint64_t[6]); uint64_t d[6]; for (int r = 0; r < 2; r++) if (S.ssbo[0][r].addr && nvn_vk_pool_describe(S.ssbo[0][r].addr, d)) fprintf(stderr, "pool(ssbo0.%d) base %#llx size %#llx abase %#llx asize %#llx gpu %#llx buf %d ", r, (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3], (unsigned long long)d[4], (int)d[5]); }
    for (int i = 0; i < S.nstream && i < 8; i++) if (S.vbuf[i].address) { uint64_t need = (uint64_t)(g_sync_imax + 1) * S.stream[i].stride; fprintf(stderr, "vbuf%d %#llx+%#llx stride %u need %llu%s ", i, (unsigned long long)S.vbuf[i].address, (unsigned long long)S.vbuf[i].size, S.stream[i].stride, (unsigned long long)need, need > S.vbuf[i].size ? " VERTEX-OUT-OF-RANGE" : ""); }
    fprintf(stderr, "\n"); fflush(stderr);
    mtl_sub_cut(); mtl_wait(mtl_last_fence(), 20ull * 1000000000ull);
}
// Range validation (Switch 2 titles; NX_NO_RANGE_CHECK=1 turns it off): a draw whose vertex indices reach past the end of a bound vertex stream would read memory the GPU may not have mapped, and a GPU
// fault takes the whole device down. The draw is dropped instead. Instance-rate and zero-stride streams, and streams bound with size 0 (unbounded), are not checked.
static int range_check_on(void) {
    static int on = -1; if (on < 0) on = (&dm_switch2_gfx && dm_switch2_gfx) && !getenv("NX_NO_RANGE_CHECK");
    return on;
}
static int range_ok(void) {
    if (g_sync_imin < 0) return 0;
    for (int i = 0; i < S.nstream && i < 16; i++) {
        if (!(g_used_streams >> i & 1) || !S.vbuf[i].address || !S.stream[i].stride || S.stream[i].divisor || !S.vbuf[i].size) continue;
        if ((uint64_t)(g_sync_imax + 1) * S.stream[i].stride > S.vbuf[i].size) return 0;
    }
    return 1;
}
extern unsigned g_cs_n;
// NX_VS_LOG=<hex prefix of a vertex shader key>: every draw with that vertex shader (its index in the submit, the draw's counts, the stage's storage rows), to find the draw that faults the GPU
static void vs_log(const char *kind, uint64_t count, int64_t first, uint32_t inst) {
    static const char *want = (const char *)-1; if (want == (const char *)-1) want = getenv("NX_VS_LOG"); if (!want) return;
    char kh[12]; snprintf(kh, sizeof kh, "%02x%02x%02x%02x", g_dbg_keys[0][0], g_dbg_keys[0][1], g_dbg_keys[0][2], g_dbg_keys[0][3]); if (!strstr(want, kh)) return;
    fprintf(stderr, "[dm] [nvn] vslog draw %u %s vs %s fs %02x%02x%02x%02x count %llu first/base %lld inst %u:", g_cs_n / 2, kind, kh, g_dbg_keys[1][0], g_dbg_keys[1][1], g_dbg_keys[1][2], g_dbg_keys[1][3], (unsigned long long)count, (long long)first, inst);
    for (int r = 0; r < 4; r++) { uint64_t a = S.ssbo[0][r].addr; uint64_t dv = a ? nvn_vk_device_address(a, S.ssbo[0][r].size ? S.ssbo[0][r].size : 1) : 0; fprintf(stderr, " | s%d %#llx size %#x dev %#llx", r, (unsigned long long)a, (unsigned)S.ssbo[0][r].size, (unsigned long long)dv); }
    for (int r = 0; r < 3; r++) { uint64_t a = S.ssbo[1][r].addr; uint64_t dv = a ? nvn_vk_device_address(a, S.ssbo[1][r].size ? S.ssbo[1][r].size : 1) : 0; fprintf(stderr, " | ps%d %#llx size %#x dev %#llx", r, (unsigned long long)a, (unsigned)S.ssbo[1][r].size, (unsigned long long)dv); }
    { const p_bind_buf *f4 = &S.ubo[1][1], *f5 = &S.ubo[1][2]; uint32_t fw = 0, fc = 0;       // the fragment program's own fetch: (c[4][0x84] & 0xfffff) + c[5][0x2ec], 32 bytes each, from its storage row 1
      if (f4->addr && f4->size > 0x88) memcpy(&fw, (void *)(uintptr_t)(f4->addr + 0x84), 4);
      if (f5->addr && f5->size > 0x2f0) memcpy(&fc, (void *)(uintptr_t)(f5->addr + 0x2ec), 4);
      uint64_t fi = (fw >> 30) == 2 ? (uint64_t)(fw & 0xfffff) + fc : 0, fo = (fw >> 30) == 2 ? fi * 32 + 0x10 : 0x10;
      fprintf(stderr, " | fsubo1 %#llx size %#x w84 %#x fsubo2 %#llx size %#x c5[2ec] %#x off %llu%s", (unsigned long long)f4->addr, (unsigned)f4->size, fw, (unsigned long long)f5->addr, (unsigned)f5->size, fc, (unsigned long long)fo, fo + 8 > S.ssbo[1][1].size ? " FS-OUT-OF-RANGE" : ""); }
    { const p_bind_buf *u4 = &S.ubo[0][1], *u5 = &S.ubo[0][2]; uint32_t w84 = 0, w88 = 0, c5 = 0;       // the fetch index of the 50 KB vertex programs: (c[4][0x88] & 0xfffff) + c[5][0x2ec] when its top bits are 10, 32 bytes a vertex
      if (u4->addr && u4->size > 0x8c) { memcpy(&w84, (void *)(uintptr_t)(u4->addr + 0x84), 4); memcpy(&w88, (void *)(uintptr_t)(u4->addr + 0x88), 4); }
      if (u5->addr && u5->size > 0x2f0) memcpy(&c5, (void *)(uintptr_t)(u5->addr + 0x2ec), 4);
      uint64_t i88 = (w88 >> 30) == 2 ? (uint64_t)(w88 & 0xfffff) + c5 : 0, i84 = (w84 >> 30) == 2 ? (uint64_t)(w84 & 0xfffff) + c5 : 0;
      fprintf(stderr, " | ubo1 %#llx size %#x c4[84] %#x c4[88] %#x c5[2ec] %#x idx %llu/%llu bytes %llu%s", (unsigned long long)u4->addr, (unsigned)u4->size, w84, w88, c5, (unsigned long long)i84, (unsigned long long)i88, (unsigned long long)(i88 * 32 + 0x20), i88 * 32 + 0x20 > S.ssbo[0][1].size ? " OUT-OF-RANGE" : ""); }
    fprintf(stderr, "\n");
}
void nvn_draw(const p_draw *d) {
    pipe_entry *pe;
    if (d->ind) { static int f; skip("indirect draws are not supported yet", &f); return; }
    g_base_instance = d->base_instance;
    { uint64_t t0 = dnow(); int ok = prepare(d->prim, &pe); atomic_fetch_add(&g_draw_prepare_ns, dnow() - t0); if (!ok) return; }
    { int sk; trace_draw("draw", (uint32_t)d->count, &sk); if (sk) return; }
    vs_log("draw", (uint64_t)d->count, (int64_t)d->first, (uint32_t)d->instances);
    g_sync_imin = d->first; g_sync_imax = (int64_t)d->first + d->count - 1;
    if (range_check_on() && !range_ok()) { static int f_rng; skip("a draw reads past the end of a vertex stream (range check)", &f_rng); return; }
    if (vs_skip()) return;
    { extern void nvn_cs_note(const uint8_t *); uint8_t k8[8] = {0}; memcpy(k8, g_dbg_keys[0], 4); nvn_cs_note(k8); memcpy(k8, g_dbg_keys[1], 4); nvn_cs_note(k8); } vkCmdDraw(nvn_vk_cmd_raw(), (uint32_t)d->count, d->instances ? (uint32_t)d->instances : 1, (uint32_t)d->first, (uint32_t)d->base_instance);
    draw_note(d->count);
    draw_sync_point("draw", (uint64_t)d->count, (int64_t)d->first, (uint32_t)d->instances);
}

void nvn_draw_idx(const p_draw_idx *d) {
    static int f_ind, f_byte;
    pipe_entry *pe;
    if (d->ind) { skip("indirect draws are not supported yet", &f_ind); return; }
    g_base_instance = d->base_instance;
    { uint64_t t0 = dnow(); int ok = prepare(d->prim, &pe); atomic_fetch_add(&g_draw_prepare_ns, dnow() - t0); if (!ok) return; }
    { int sk; trace_draw("indexed", (uint32_t)d->count, &sk); if (sk) return; }
    vs_log("indexed", (uint64_t)d->count, (int64_t)d->base_vertex, (uint32_t)d->instances);
    { static int sy = -1; if (sy < 0) sy = getenv("NX_DRAW_SYNC_FROM") != NULL;       // the index range of this draw (plus base vertex), read from the index buffer
      int chk = range_check_on();
      g_sync_imin = g_sync_imax = 0;
      if ((sy || chk) && d->ibuf && d->count > 0 && d->count < 1000000) {
          int64_t mn = INT64_MAX, mx = -1; uint64_t n = (uint64_t)d->count;
          for (uint64_t i = 0; i < n; i++) { int64_t v = d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE ? ((const uint8_t *)(uintptr_t)d->ibuf)[i] : d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? ((const uint16_t *)(uintptr_t)d->ibuf)[i] : ((const uint32_t *)(uintptr_t)d->ibuf)[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
          g_sync_imin = mn + d->base_vertex; g_sync_imax = mx + d->base_vertex; }
      if (chk && !range_ok()) { static int f_rng; skip("a draw reads past the end of a vertex stream (range check)", &f_rng); return; } }
    if (vs_skip()) return;
    VkCommandBuffer cb = nvn_vk_cmd_raw(); VkBuffer ib; VkDeviceSize ioff;
    if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {
        // Vulkan's core index types are 16 and 32 bits: the bytes are widened into a piece of the transient arena (read now, as the other indices are by the GPU when the draw runs)
        uint64_t n = (uint64_t)d->count;
        VkDescriptorBufferInfo wi; void *wp;
        if (!n || !d->ibuf || nvn_vk_transient(2 * n, &wi, &wp)) { skip("the 8-bit indices cannot be widened (no index data or no transient space)", &f_byte); return; }
        const uint8_t *src = (const uint8_t *)(uintptr_t)d->ibuf; uint16_t *dst = wp;
        for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
        vkCmdBindIndexBuffer(cb, wi.buffer, wi.offset, VK_INDEX_TYPE_UINT16);
        { extern void nvn_cs_note(const uint8_t *); uint8_t k8[8] = {0}; memcpy(k8, g_dbg_keys[0], 4); nvn_cs_note(k8); memcpy(k8, g_dbg_keys[1], 4); nvn_cs_note(k8); } vkCmdDrawIndexed(cb, (uint32_t)d->count, d->instances ? (uint32_t)d->instances : 1, 0, d->base_vertex, (uint32_t)d->base_instance);
        draw_note(d->count);
        draw_sync_point("indexed8", (uint64_t)d->count, (int64_t)d->base_vertex, (uint32_t)d->instances);
        return;
    }
    uint64_t isz = d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4;
    if (!nvn_vk_pool_buffer(d->ibuf, isz * (uint64_t)d->count, &ib, &ioff)) { static int f; skip("the index buffer lies outside the memory pools", &f); return; }
    vkCmdBindIndexBuffer(cb, ib, ioff, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    { extern void nvn_cs_note(const uint8_t *); uint8_t k8[8] = {0}; memcpy(k8, g_dbg_keys[0], 4); nvn_cs_note(k8); memcpy(k8, g_dbg_keys[1], 4); nvn_cs_note(k8); } vkCmdDrawIndexed(cb, (uint32_t)d->count, d->instances ? (uint32_t)d->instances : 1, 0, d->base_vertex, (uint32_t)d->base_instance);
    draw_note(d->count);
    draw_sync_point("indexed", (uint64_t)d->count, (int64_t)d->base_vertex, (uint32_t)d->instances);
}
