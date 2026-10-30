// Replay of the command records hle_nvn.c writes into the game's command memory (see "Command records" in nvn_obj.h). Port of the
// walking loop of nxnative's mtl_exec (nvn_mtl.m) onto the Vulkan backend.
//
// Implemented: the list structure (END, JUMP, CALL), the operations with a visible effect outside the GPU (events, counters, sync fences),
// buffer copies, fills and uniform updates, and barriers. The operations that draw (render targets, clears, state, draws, dispatches,
// texture copies) are accepted and counted but not executed yet; each kind is announced once so progress is visible in the log.
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_vk.h"
#include "nvn_vk_priv.h"
#include "nvn_draw.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define N(x) [x] = #x
static const char *const op_name[OP_COUNT_] = {
    N(OP_END), N(OP_JUMP), N(OP_BEGIN), N(OP_CALL), N(OP_SIGNAL_EVENT), N(OP_REPORT_COUNTER), N(OP_COPY_B2B), N(OP_UPDATE_UB),
    N(OP_SET_RT), N(OP_CLEAR_COLOR), N(OP_CLEAR_DS), N(OP_VIEWPORTS), N(OP_SCISSOR), N(OP_DEPTH_RANGE),
    N(OP_BIND_PROGRAM), N(OP_BIND_STATE), N(OP_BIND_VATTR), N(OP_BIND_VSTREAM), N(OP_BIND_VBUF),
    N(OP_BIND_UBO), N(OP_BIND_SSBO), N(OP_BIND_TEX), N(OP_BIND_SAMP),
    N(OP_STENCIL), N(OP_POLY_OFFSET), N(OP_DRAW), N(OP_DRAW_IDX), N(OP_DISPATCH), N(OP_BARRIER),
    N(OP_SET_TEX_POOL), N(OP_SET_SAMP_POOL), N(OP_COPY_B2T), N(OP_COPY_STRIDE), N(OP_BLEND_COLOR), N(OP_COPY_T2B), N(OP_BIND_IMG), N(OP_COPY_T2T),
    N(OP_RESET_COUNTER), N(OP_CLEAR_TEX), N(OP_CLEAR_BUF), N(OP_DEPTH_CLAMP), N(OP_RENDER_COND), N(OP_FENCE_SYNC), N(OP_TF_BIND), N(OP_TF_STATE),
    N(OP_RAST_DISCARD), N(OP_VP_SWIZZLE), N(OP_BIND_TEX1), N(OP_PRIM_RESTART), N(OP_BIND_UBO_SMALL), N(OP_BIND_VBUF1),
};
#undef N

static _Atomic uint64_t g_seen[OP_COUNT_], g_pending[OP_COUNT_];
// Per-frame averages since the last report of the ops replayed most, render passes begun, full barriers, where the host time goes (replay, driver submit, a
// draw's prepare and its parts, barrier recording), pipeline lookups and the texture table (the timing line calls it)
void nvn_replay_report(uint64_t frames) {
    static uint64_t last[OP_COUNT_], lpass, lbar, lrep, lcom, llk, llast, lnew, lprep, lpipe, lbuf, lps, lbns;
    extern _Atomic uint64_t g_nvn_npass, g_nvn_nbarrier_emitted;
    if (!frames) return;
    uint64_t d[OP_COUNT_]; for (unsigned i = 0; i < OP_COUNT_; i++) { uint64_t v = atomic_load(&g_seen[i]); d[i] = v - last[i]; last[i] = v; }
    uint64_t p = atomic_load(&g_nvn_npass), b = atomic_load(&g_nvn_nbarrier_emitted);
    extern _Atomic uint64_t g_replay_ns, g_commit_ns;
    uint64_t rep = atomic_load(&g_replay_ns), com = atomic_load(&g_commit_ns);
    extern _Atomic uint64_t g_draw_prepare_ns, g_draw_pipe_ns, g_draw_buf_ns, g_draw_pass_ns, g_nvn_barrier_ns;
    uint64_t prep = atomic_load(&g_draw_prepare_ns), pipe = atomic_load(&g_draw_pipe_ns), buf = atomic_load(&g_draw_buf_ns), pss = atomic_load(&g_draw_pass_ns), bns = atomic_load(&g_nvn_barrier_ns);
    void nvn_draw_pipe_stats(uint64_t *, uint64_t *, uint64_t *); uint64_t look, lastk, made; nvn_draw_pipe_stats(&look, &lastk, &made);
    void mtl_tex_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *); uint64_t tl, td, tdb, tsw; mtl_tex_stats(&tl, &td, &tdb, &tsw);
    char line[1600]; size_t o = 0;
    o += snprintf(line + o, sizeof line - o, "per frame: %.0f render passes, %.0f full barriers; replay %.2f ms, submit %.2f ms; draws %.2f ms (pipeline %.2f, driver buffers %.2f, pass %.2f), barriers %.2f ms; pipeline lookups %.0f (%.0f%% last key, %.0f new, %llu built); textures %llu live, %llu dead, %llu MB dead, %llu swept;",
                  (double)(p - lpass) / frames, (double)(b - lbar) / frames, (double)(rep - lrep) / frames / 1e6, (double)(com - lcom) / frames / 1e6,
                  (double)(prep - lprep) / frames / 1e6, (double)(pipe - lpipe) / frames / 1e6, (double)(buf - lbuf) / frames / 1e6, (double)(pss - lps) / frames / 1e6,
                  (double)(bns - lbns) / frames / 1e6,
                  (double)(look - llk) / frames, look - llk ? 100.0 * (double)(lastk - llast) / (double)(look - llk) : 0.0, (double)(made - lnew) / frames,
                  (unsigned long long)made, (unsigned long long)tl, (unsigned long long)td, (unsigned long long)(tdb >> 20), (unsigned long long)tsw);
    lpass = p; lbar = b; lrep = rep; lcom = com; llk = look; llast = lastk; lnew = made;
    lprep = prep; lpipe = pipe; lbuf = buf; lps = pss; lbns = bns;
    for (int k = 0; k < 10; k++) {
        unsigned best = 0; for (unsigned i = 1; i < OP_COUNT_; i++) if (d[i] > d[best]) best = i;
        if (!d[best]) break;
        o += snprintf(line + o, sizeof line - o, " %s %.0f", op_name[best] ? op_name[best] : "?", (double)d[best] / frames); d[best] = 0;
    }
    fprintf(stderr, "[dm] [nvn] replay %s\n", line);
}
uint64_t nvn_replay_ops(unsigned op) { return op < OP_COUNT_ ? atomic_load(&g_seen[op]) : 0; }          // how many times an op was replayed
uint64_t nvn_replay_skipped(unsigned op) { return op < OP_COUNT_ ? atomic_load(&g_pending[op]) : 0; }   // ... and how many of those did nothing yet

uint64_t nvn_replay_ops_named(const char *name) {      // the same by op name ("OP_COPY_B2B"), for callers that cannot include nvn_obj.h (it is C only)
    for (unsigned i = 0; i < OP_COUNT_; i++) if (op_name[i] && !strcmp(op_name[i], name)) return atomic_load(&g_seen[i]);
    return 0;
}
static uint64_t g_copy_row, g_copy_img;                // OP_COPY_STRIDE state: bytes per row and per image of buffer<->texture copies (0 = tight)
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }               // lookahead across handles: only the Metal pass merging uses it
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }

static void pending(unsigned op) {                                                // an operation this replay does not execute yet
    uint64_t n = atomic_fetch_add(&g_pending[op], 1) + 1;
    if (n == 1 || (n >= 1000 && !(n & (n - 1)))) fprintf(stderr, "[dm] [nvn] replay: %s is recorded but not executed yet (%llu so far)\n", op_name[op] ? op_name[op] : "?", (unsigned long long)n);   // the count: how much of the frame never ran
}

static void update_ub(uint64_t dst, uint64_t n, const uint8_t *data) {          // OP_UPDATE_UB: the data was copied into the record at record time
    vkctx_t *c = nvn_vk_ctx(); VkCommandBuffer cb = nvn_vk_cmd();
    if (!c || !cb || !n) return;
    VkBuffer b; VkDeviceSize off;
    if (!nvn_vk_pool_buffer(dst, n, &b, &off)) { fprintf(stderr, "[dm] [RED] [nvn] uniform update of %#llx (%#llx bytes) is outside the memory pools\n", (unsigned long long)dst, (unsigned long long)n); return; }
    if (n <= 65536 && !(n & 3) && !(off & 3)) vkCmdUpdateBuffer(cb, b, off, n, data);   // copied into the command buffer now: the record may be reused
    else {                                                                              // any size and alignment: through a staging buffer the submission keeps alive
        vkbuf_t st;
        if (vkbuf_create(c, &st, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1)) return;
        memcpy(st.map, data, n);
        VkBufferCopy rg = {0, off, n};
        vkCmdCopyBuffer(cb, st.buf, b, 1, &rg);
        nvn_vk_keep(&st);
    }
    nvn_vk_barrier_all(cb);
}

void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7)) { fprintf(stderr, "[dm] [RED] [nvn] replay: a command record at %p has size %u (op %u): the list is damaged, stopping\n", (const void *)p, h.size, h.op); return; }
        const uint8_t *a = p + 8;
        if (h.op < OP_COUNT_) atomic_fetch_add(&g_seen[h.op], 1);
        switch (h.op) {
        case OP_END: return;
        case OP_BEGIN: break;
        case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)(uintptr_t)t); break; }
        case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
        case OP_RESET_COUNTER: break;                                              // only the samples-passed counter has state to reset
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = nvn_vk_next_fence(); s->fenced = 1; break; }   // the fence value this submission commits with
        case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
        case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s_, d, n); break; }
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); update_ub(d, n, a + 16); break; }
        case OP_BARRIER: { VkCommandBuffer cb = nvn_vk_cmd_raw(); if (!cb) break; if (nvn_draw_pass_active()) nvn_vk_barrier_mark(); else { nvn_vk_barrier_flush(cb); nvn_vk_barrier_all(cb); } break; }   // a full barrier covers every combination of the game's bits; inside a pass it cannot be recorded, so it waits for the pass to end
        case OP_COPY_STRIDE: { const p_copy_stride *cs = (const p_copy_stride *)a; if (cs->image) g_copy_img = (uint64_t)cs->stride; else g_copy_row = (uint64_t)cs->stride; break; }
        case OP_COPY_B2T: nvn_vk_copy_b2t((const p_copy_b2t *)a, g_copy_row, g_copy_img); break;
        case OP_COPY_T2B: nvn_vk_copy_t2b((const p_copy_b2t *)a, g_copy_row, g_copy_img); break;   // the record is a p_copy_b2t whose src is the destination buffer
        case OP_SET_RT: {
            const p_set_rt *r = (const p_set_rt *)a;
            memset(&g_rt, 0, sizeof g_rt); g_rt.n = r->n > 8 ? 8 : r->n;
            for (int i = 0; i < g_rt.n; i++) { g_rt.cser[i] = r->color[i] ? r->color[i]->serial : 0; g_rt.cview[i] = r->cview[i]; g_rt.has_cview[i] = r->has_cview[i]; }
            g_rt.dser = r->depth ? r->depth->serial : 0; g_rt.dview = r->dview; g_rt.has_dview = r->has_dview;
            break; }
        case OP_SCISSOR: { const p_scissor *sc = (const p_scissor *)a; g_sc.has = 1; g_sc.r[0] = sc->x; g_sc.r[1] = sc->y; g_sc.r[2] = sc->w; g_sc.r[3] = sc->h; break; }
        case OP_DRAW: { p_draw t; nvn_draw(draw_rec(a, h.size, &t)); break; }
        case OP_DRAW_IDX: { p_draw_idx t; nvn_draw_idx(draw_idx_rec(a, h.size, &t)); break; }
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1: case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SSBO:
        case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_STENCIL: case OP_PRIM_RESTART: case OP_RAST_DISCARD:
            nvn_draw_state_op(h.op, a); break;
        case OP_CLEAR_COLOR: {
            const p_clear_color *cc = (const p_clear_color *)a;
            if (cc->index < 0 || cc->index >= g_rt.n || !g_rt.cser[cc->index]) { fprintf(stderr, "[dm] [RED] [nvn] colour clear of target %d, which is not set\n", cc->index); break; }
            nvn_vk_clear_color(g_rt.cser[cc->index], &g_rt.cview[cc->index], g_rt.has_cview[cc->index], cc->mask, cc->rgba, g_sc.has ? g_sc.r : NULL);
            break; }
        case OP_CLEAR_DS: {
            const p_clear_ds *cd = (const p_clear_ds *)a;
            if (!g_rt.dser) { fprintf(stderr, "[dm] [RED] [nvn] depth-stencil clear with no depth target set\n"); break; }
            nvn_vk_clear_ds(g_rt.dser, &g_rt.dview, g_rt.has_dview, cd->depth, cd->depth_mask, cd->stencil, cd->stencil_mask, g_sc.has ? g_sc.r : NULL);
            break; }
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;                        // descriptor pool state: only a draw or a dispatch reads it
        default:
            if (h.op < OP_COUNT_) pending(h.op);
            else fprintf(stderr, "[dm] [RED] [nvn] replay: unknown command record %u\n", h.op);
            break;
        }
        p += h.size;
    }
}
