// Textures on Vulkan, host side. A texture's storage is written in the Tegra's block-linear layout (nvn_swizzle), the texture is registered,
// and recorded texture->buffer copies make the GPU read it back linear: every level and layer of 2D, compressed, array, 3D and cube textures
// must equal the data that was swizzled in. Then buffer->texture copies with a padded row stride, the CPU's ReadTexels on both owners of the
// truth (storage and image), and the CPU writing a texture the GPU had written.
#include "../src/host/guest_mem.h"
#include "../src/gfx/nvn/nvn_obj.h"
#include "../src/gfx/nvn/nvn_mtl.h"
#include "../src/gfx/nvn/nvn_vk.h"
#include "../src/gfx/tex/nvn_deswizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static uint8_t *g_storage, *g_buf;                       // two pools: texture storage, linear buffers
static uint64_t g_serial = 1000;
static uint8_t *g_list, *g_w;

static void emit(uint32_t op, const void *pl, size_t n) { op_hdr h = {op, (uint32_t)((8 + n + 7) & ~7u)}; memcpy(g_w, &h, 8); if (n) memcpy(g_w + 8, pl, n); g_w += h.size; }
static void list_begin(void) { g_w = g_list; }
static void list_run(void) {
    emit(OP_END, NULL, 0);
    mtl_sub_begin(); mtl_exec(g_list); uint64_t f = mtl_sub_commit();
    CHECK(mtl_wait(f, 10000000000ull));
}

typedef struct { const char *name; int target, format; uint32_t w, h, depth, levels; } tcase;
typedef struct { tex_t t; uint8_t *lin[64]; size_t pitch[64], size[64]; int nl, nly; } tcase_data;

static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }

// Registers a texture whose storage holds `lin` swizzled in; fills lin[level * nly + layer]
static void make(tcase_data *td, const tcase *c, uint8_t *storage) {
    memset(td, 0, sizeof *td);
    td->t.d = (nvn_texdesc){.target = c->target, .format = c->format, .width = c->w, .height = c->h, .depth = c->depth, .levels = c->levels};
    nvn_tex_layout(&td->t.d, &td->t.L);
    td->t.mem = storage; td->t.serial = ++g_serial;
    memset(storage, 0, td->t.L.size);
    int bw, bh, bpb = nvn_fmt_bpb(c->format, &bw, &bh);
    int is3d = c->target == NVN_TEXTURE_TARGET_3D;
    td->nly = is3d ? 1 : td->t.L.layers ? td->t.L.layers : 1; td->nl = c->levels;
    for (int lv = 0; lv < c->levels; lv++) {
        uint32_t lw = c->w >> lv ? c->w >> lv : 1, lh = c->h >> lv ? c->h >> lv : 1, lz = is3d ? (c->depth >> lv ? c->depth >> lv : 1) : 1;
        size_t pitch = (size_t)((lw + bw - 1) / bw) * bpb, rows = (lh + bh - 1) / bh;
        for (int ly = 0; ly < td->nly; ly++) {
            int k = lv * td->nly + ly;
            td->pitch[k] = pitch; td->size[k] = pitch * rows * lz; td->lin[k] = malloc(td->size[k]);
            for (size_t i = 0; i < td->size[k]; i++) td->lin[k][i] = (uint8_t)mix((uint32_t)(i * 31 + lv * 977 + ly * 131 + c->format));
            CHECK(nvn_swizzle(&td->t.d, lv, ly, td->lin[k], pitch, storage) == 0);
        }
    }
    mtl_tex_init(&td->t); mtl_tex_cpu_written(&td->t);
}

static p_copy_b2t rec(const tex_t *t, uint64_t addr, int level, int layer, int x, int y, int z, int w, int h, int d) {
    p_copy_b2t c; memset(&c, 0, sizeof c);
    c.src = addr; c.tex = t; c.has_view = 1; c.view.set_levels = 1; c.view.base_level = (uint8_t)level; c.view.set_layers = 1; c.view.min_layer = (uint16_t)layer;
    c.r = (NVNcopyRegion){x, y, z, w, h, d};
    return c;
}

static int check_all(tcase_data *td, const tcase *c, const char *tag) {
    int bad = 0, is3d = c->target == NVN_TEXTURE_TARGET_3D;
    list_begin();
    size_t off = 0, offs[64];
    for (int lv = 0; lv < td->nl; lv++) for (int ly = 0; ly < td->nly; ly++) {
        int k = lv * td->nly + ly; off = (off + 255) & ~255ull; offs[k] = off;
        uint32_t lw = c->w >> lv ? c->w >> lv : 1, lh = c->h >> lv ? c->h >> lv : 1, lz = is3d ? (c->depth >> lv ? c->depth >> lv : 1) : 1;
        p_copy_b2t r = rec(&td->t, (uint64_t)(uintptr_t)(g_buf + off), lv, ly, 0, 0, 0, (int)lw, (int)lh, (int)lz);
        emit(OP_COPY_T2B, &r, sizeof r);
        off += td->size[k];
    }
    list_run();
    for (int k = 0; k < td->nl * td->nly; k++) if (memcmp(g_buf + offs[k], td->lin[k], td->size[k])) { bad++; printf("  %s: level %d layer %d differs\n", tag, k / td->nly, k % td->nly); }
    return bad;
}

int main(void) {
    if (guest_mem_init(512ull << 20)) return 1;
    if (!nvn_gpu_ok()) { puts("SKIP: no Vulkan device"); return 0; }
    g_storage = (uint8_t *)guest_alloc(8u << 20, 0x10000); g_buf = (uint8_t *)guest_alloc(4u << 20, 0x10000);
    g_list = (uint8_t *)guest_alloc(0x10000, 16);
    mtl_pool_add(g_storage, 8u << 20); mtl_pool_add(g_buf, 4u << 20);

    // NVN formats: 0x01 R8, 0x25 RGBA8, 0x29 RGBA16F, 0x42 RGBA_DXT1 (BC1), 0x4D BPTC (BC7)
    static const tcase cases[] = {
        {"RGBA8 70x37 4 levels", NVN_TEXTURE_TARGET_2D, 0x25, 70, 37, 1, 4},
        {"BC1 64x40 3 levels", NVN_TEXTURE_TARGET_2D, 0x42, 64, 40, 1, 3},
        {"BC7 30x22 3 levels (edge blocks)", NVN_TEXTURE_TARGET_2D, 0x4D, 30, 22, 1, 3},
        {"R8 array 16x16x4 3 levels", NVN_TEXTURE_TARGET_2D_ARRAY, 0x01, 16, 16, 4, 3},
        {"RGBA16F 3D 8x8x4 3 levels", NVN_TEXTURE_TARGET_3D, 0x29, 8, 8, 4, 3},
        {"RGBA8 cube 16x16 2 levels", NVN_TEXTURE_TARGET_CUBEMAP, 0x25, 16, 16, 6, 2},
    };
    tcase_data td[6];
    for (int i = 0; i < 6; i++) {
        make(&td[i], &cases[i], g_storage + (size_t)i * (1u << 20));
        int bad = check_all(&td[i], &cases[i], cases[i].name);
        printf("%-34s %u levels x %d layer(s): %s\n", cases[i].name, cases[i].levels, td[i].nly, bad ? "MISMATCH" : "ok");
        CHECK(!bad);
    }

    // Buffer->texture with a padded row stride, into the middle of level 1 of the first texture
    tcase_data *a = &td[0];
    {
        uint8_t *src = g_buf + 0x100000; int pw = 16, ph = 8; size_t stride = 80;       // 16 RGBA8 texels = 64 bytes of a row, padded to 80
        for (int y = 0; y < ph; y++) for (int x = 0; x < (int)stride; x++) src[y * stride + x] = (x < pw * 4) ? (uint8_t)(0xA0 + y + x) : 0xEE;
        list_begin();
        p_copy_stride st = {(int64_t)stride, 0}; emit(OP_COPY_STRIDE, &st, sizeof st);
        p_copy_b2t r = rec(&a->t, (uint64_t)(uintptr_t)src, 1, 0, 3, 2, 0, pw, ph, 1); emit(OP_COPY_B2T, &r, sizeof r);
        p_copy_stride st0 = {0, 0}; emit(OP_COPY_STRIDE, &st0, sizeof st0);
        list_run();
        CHECK(mtl_tex_gpu_owned(a->t.serial));                                           // the image now holds what the storage does not
        uint8_t *want = malloc(a->size[1 * a->nly]); memcpy(want, a->lin[1 * a->nly], a->size[1 * a->nly]);
        size_t p1 = a->pitch[1 * a->nly];
        for (int y = 0; y < ph; y++) memcpy(want + (size_t)(2 + y) * p1 + 3 * 4, src + y * stride, (size_t)pw * 4);
        free(a->lin[1 * a->nly]); a->lin[1 * a->nly] = want;                               // the expected level 1: the original with the patch laid over it
        int bad = check_all(a, &cases[0], "after the patch");
        printf("buffer->texture, padded stride: %s\n", bad ? "MISMATCH" : "ok");
        CHECK(!bad);                                                                       // every other level is still the original

        // CPU read of a texture the GPU wrote: the image is read back
        uint8_t out[16 * 8 * 4];
        NVNcopyRegion rr = {3, 2, 0, pw, ph, 1};
        mtl_read_texels(&a->t, 1, 0, &rr, out, 0, 0);
        int rok = 1; for (int y = 0; y < ph; y++) if (memcmp(out + y * pw * 4, src + y * stride, (size_t)pw * 4)) rok = 0;
        printf("ReadTexels of a GPU-written texture (readback): %s\n", rok ? "ok" : "MISMATCH"); CHECK(rok);
    }

    // CPU read of a texture the GPU did not write: de-swizzled from the storage, a sub-rectangle with its own strides
    {
        tcase_data *b = &td[1];                                                            // BC1 64x40, level 0: 16x10 blocks of 8 bytes
        uint8_t out[3 * 2 * 8 + 4 * 64];
        memset(out, 0xCC, sizeof out);
        NVNcopyRegion rr = {8, 8, 0, 12, 8, 1};                                            // 3x2 blocks starting at block (2,2)
        mtl_read_texels(&b->t, 0, 0, &rr, out, 64, 0);                                     // rows 64 bytes apart
        int rok = 1;
        for (int y = 0; y < 2; y++) if (memcmp(out + y * 64, b->lin[0] + (size_t)(2 + y) * b->pitch[0] + 2 * 8, 3 * 8)) rok = 0;
        printf("ReadTexels of a storage texture (strided sub-rectangle of blocks): %s\n", rok ? "ok" : "MISMATCH"); CHECK(rok);
        CHECK(!mtl_tex_gpu_owned(b->t.serial));
    }

    // The CPU writes level 0 of the texture the GPU had written level 1 of: level 0 is filled from the storage again, level 1 keeps what the GPU wrote
    {
        uint8_t *nw = malloc(a->size[0]);
        for (size_t i = 0; i < a->size[0]; i++) nw[i] = (uint8_t)mix((uint32_t)i + 12345);
        CHECK(nvn_swizzle(&a->t.d, 0, 0, nw, a->pitch[0], g_storage) == 0);
        mtl_tex_cpu_written_sub(&a->t, 0, 0, 1);
        CHECK(mtl_tex_gpu_owned(a->t.serial));                                              // level 1 is still the GPU's
        free(a->lin[0]); a->lin[0] = nw;
        int bad = check_all(a, &cases[0], "after the CPU rewrote level 0");                // level 0 new, level 1 patched, levels 2 and 3 original
        printf("CPU rewrite of one level, the GPU's other level kept: %s\n", bad ? "MISMATCH" : "ok");
        CHECK(!bad);

        // ... and the whole-texture call, for a caller that does not know what it wrote, takes the storage back for every level
        mtl_tex_cpu_written(&a->t);
        CHECK(!mtl_tex_gpu_owned(a->t.serial));
        uint8_t *orig1 = malloc(a->size[1 * a->nly]);
        CHECK(nvn_deswizzle(&a->t.d, 1, 0, g_storage, orig1, a->pitch[1 * a->nly]) == 0);   // what the storage holds for level 1: the original, not the GPU's patch
        free(a->lin[1 * a->nly]); a->lin[1 * a->nly] = orig1;
        bad = check_all(a, &cases[0], "after a whole-texture CPU write");
        printf("whole-texture CPU write refills every level from the storage: %s\n", bad ? "MISMATCH" : "ok");
        CHECK(!bad);
    }

    // ---- render targets and clears
    {
        uint8_t *cst = g_storage + (size_t)7 * (1u << 20);            // render targets need no storage contents
        tex_t rt, srgb, ui, arr, dep, sten, pm;
        #define NEWTEX(t, tgt, fmt, w_, h_, d_, lv_) do { memset(&(t), 0, sizeof(t)); (t).d = (nvn_texdesc){.target = (tgt), .format = (fmt), .width = (w_), .height = (h_), .depth = (d_), .levels = (lv_)}; \
            nvn_tex_layout(&(t).d, &(t).L); (t).mem = cst; (t).serial = ++g_serial; mtl_tex_init(&(t)); } while (0)
        NEWTEX(rt, NVN_TEXTURE_TARGET_2D, 0x25, 64, 32, 1, 3);
        NEWTEX(srgb, NVN_TEXTURE_TARGET_2D, 0x25, 16, 16, 1, 1);
        NEWTEX(ui, NVN_TEXTURE_TARGET_2D, 0x0B, 16, 16, 1, 1);
        NEWTEX(arr, NVN_TEXTURE_TARGET_2D_ARRAY, 0x01, 8, 8, 4, 1);
        NEWTEX(dep, NVN_TEXTURE_TARGET_2D, 0x34, 32, 32, 1, 1);
        NEWTEX(sten, NVN_TEXTURE_TARGET_2D, 0x31, 16, 16, 1, 1);
        NEWTEX(pm, NVN_TEXTURE_TARGET_2D, 0x25, 16, 16, 1, 1);

        #define SETRT(t_, hasv, setl, lvl, setlay, l0, nl, setf, f) do { p_set_rt r_; memset(&r_, 0, sizeof r_); r_.n = 1; r_.color[0] = &(t_); r_.has_cview[0] = (hasv); \
            r_.cview[0].set_levels = (setl); r_.cview[0].base_level = (lvl); r_.cview[0].set_layers = (setlay); r_.cview[0].min_layer = (l0); r_.cview[0].nlayers = (nl); \
            r_.cview[0].set_format = (setf); r_.cview[0].format = (f); emit(OP_SET_RT, &r_, sizeof r_); } while (0)
        #define CLEARC(idx, msk, r_, g_, b_, a_) do { p_clear_color c_ = {(idx), (msk), {(r_), (g_), (b_), (a_)}}; emit(OP_CLEAR_COLOR, &c_, sizeof c_); } while (0)
        #define SCISSOR(x_, y_, w_, h_) do { p_scissor sc_ = {(x_), (y_), (w_), (h_)}; emit(OP_SCISSOR, &sc_, sizeof sc_); } while (0)
        #define READBACK(t_, lvl, lay, w_, h_, depth_, dst_) do { p_copy_b2t c_ = rec(&(t_), (uint64_t)(uintptr_t)(dst_), (lvl), (lay), 0, 0, 0, (w_), (h_), (depth_)); emit(OP_COPY_T2B, &c_, sizeof c_); } while (0)
        uint8_t *rb = g_buf + 0x200000;
        #define NEAR(a_, b_, tol_) ((a_) >= (b_) - (tol_) && (a_) <= (b_) + (tol_))

        // 1. a full clear, then a scissored one, then another level through a view
        list_begin(); SETRT(rt, 0, 0, 0, 0, 0, 0, 0, 0); CLEARC(0, 0xF, 1.0f, 0.5f, 0.25f, 1.0f); READBACK(rt, 0, 0, 64, 32, 1, rb); list_run();
        int ok = 1; for (int i = 0; i < 64 * 32; i++) ok &= rb[i * 4] == 255 && NEAR(rb[i * 4 + 1], 128, 1) && NEAR(rb[i * 4 + 2], 64, 1) && rb[i * 4 + 3] == 255;
        printf("full colour clear: %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);
        list_begin(); SCISSOR(10, 5, 20, 8); CLEARC(0, 0xF, 0.0f, 0.0f, 1.0f, 1.0f); READBACK(rt, 0, 0, 64, 32, 1, rb); list_run();
        ok = 1;
        for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++) {
            const uint8_t *q = rb + (y * 64 + x) * 4; int in = x >= 10 && x < 30 && y >= 5 && y < 13;
            ok &= in ? (q[0] == 0 && q[1] == 0 && q[2] == 255) : (q[0] == 255 && NEAR(q[1], 128, 1));
        }
        printf("scissored colour clear (only the rectangle): %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);
        list_begin(); SCISSOR(0, 0, 64, 32); SETRT(rt, 1, 1, 1, 0, 0, 0, 0, 0); CLEARC(0, 0xF, 0.0f, 1.0f, 0.0f, 1.0f);      // level 1 only (32x16)
        READBACK(rt, 1, 0, 32, 16, 1, rb); READBACK(rt, 0, 0, 64, 32, 1, rb + 0x10000); list_run();
        ok = 1; for (int i = 0; i < 32 * 16; i++) ok &= rb[i * 4] == 0 && rb[i * 4 + 1] == 255 && rb[i * 4 + 2] == 0;
        const uint8_t *l0 = rb + 0x10000; ok &= l0[(6 * 64 + 12) * 4 + 2] == 255 && l0[0] == 255;                              // level 0 keeps the earlier clears
        printf("clear of one mip level through a view: %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);
        CHECK(mtl_tex_gpu_owned(rt.serial));
        NVNcopyRegion pr = {0, 0, 0, 2, 1, 1}; uint8_t px2[8];
        mtl_read_texels(&rt, 1, 0, &pr, px2, 0, 0);
        CHECK(px2[0] == 0 && px2[1] == 255 && px2[2] == 0 && px2[4] == 0 && px2[5] == 255);                                    // ReadTexels sees the clear

        // 2. an sRGB view of an RGBA8 texture: the linear clear value is stored encoded
        list_begin(); SETRT(srgb, 1, 0, 0, 0, 0, 0, 1, 0x38); CLEARC(0, 0xF, 0.5f, 0.5f, 0.5f, 1.0f); READBACK(srgb, 0, 0, 16, 16, 1, rb); list_run();
        ok = 1; for (int i = 0; i < 256; i++) ok &= NEAR(rb[i * 4], 188, 2) && NEAR(rb[i * 4 + 1], 188, 2) && NEAR(rb[i * 4 + 2], 188, 2) && rb[i * 4 + 3] == 255;
        printf("clear through an sRGB view (0.5 linear -> %u): %s\n", rb[0], ok ? "ok" : "MISMATCH"); CHECK(ok);

        // 3. an integer target: the float the game recorded becomes the integer
        list_begin(); SETRT(ui, 0, 0, 0, 0, 0, 0, 0, 0); CLEARC(0, 0xF, 7.0f, 0.0f, 0.0f, 0.0f); READBACK(ui, 0, 0, 16, 16, 1, rb); list_run();
        ok = 1; for (int i = 0; i < 256; i++) ok &= ((uint32_t *)rb)[i] == 7;
        printf("integer clear (R32_UINT = 7): %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);

        // 4. layers of an array: all four to 0, then layer 2 only to 0.5
        list_begin(); SETRT(arr, 1, 0, 0, 1, 0, 4, 0, 0); CLEARC(0, 0xF, 0.0f, 0.0f, 0.0f, 0.0f);
        SETRT(arr, 1, 0, 0, 1, 2, 1, 0, 0); CLEARC(0, 0xF, 0.5f, 0.0f, 0.0f, 0.0f);
        for (int l = 0; l < 4; l++) READBACK(arr, 0, l, 8, 8, 1, rb + l * 256);
        list_run();
        ok = 1; for (int l = 0; l < 4; l++) for (int i = 0; i < 64; i++) ok &= (l == 2) ? NEAR(rb[l * 256 + i], 128, 1) : rb[l * 256 + i] == 0;
        printf("clear of array layers (all, then layer 2): %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);

        // 5. depth: whole, then a scissor; stencil
        {
            p_set_rt r_; memset(&r_, 0, sizeof r_); r_.n = 0; r_.depth = &dep;
            list_begin(); SCISSOR(0, 0, 32, 32); emit(OP_SET_RT, &r_, sizeof r_);
            p_clear_ds d1 = {0.25f, 1, 0, 0}; emit(OP_CLEAR_DS, &d1, sizeof d1);
            SCISSOR(4, 4, 8, 8); p_clear_ds d2 = {0.75f, 1, 0, 0}; emit(OP_CLEAR_DS, &d2, sizeof d2);
            READBACK(dep, 0, 0, 32, 32, 1, rb); list_run();
            ok = 1; for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { float f = ((float *)rb)[y * 32 + x]; int in = x >= 4 && x < 12 && y >= 4 && y < 12; ok &= f == (in ? 0.75f : 0.25f); }
            printf("depth clear, whole then scissored: %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);
            p_set_rt r2; memset(&r2, 0, sizeof r2); r2.n = 0; r2.depth = &sten;
            list_begin(); SCISSOR(0, 0, 16, 16); emit(OP_SET_RT, &r2, sizeof r2);
            p_clear_ds d3 = {1.0f, 0, 0x5A, 0xFF}; emit(OP_CLEAR_DS, &d3, sizeof d3);
            READBACK(sten, 0, 0, 16, 16, 1, rb); list_run();
            ok = 1; for (int i = 0; i < 256; i++) ok &= rb[i] == 0x5A;
            printf("stencil clear (S8 = 0x5A): %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);
        }

        // 6. a clear with a channel mask is not executed yet: it must leave the target alone (and say so)
        list_begin(); SETRT(pm, 0, 0, 0, 0, 0, 0, 0, 0); CLEARC(0, 0xF, 1.0f, 0.0f, 0.0f, 1.0f); CLEARC(0, 0x3, 0.0f, 1.0f, 0.0f, 1.0f); READBACK(pm, 0, 0, 16, 16, 1, rb); list_run();
        ok = 1; for (int i = 0; i < 256; i++) ok &= rb[i * 4] == 255 && rb[i * 4 + 1] == 0;
        printf("clear with a channel mask leaves the target alone: %s\n", ok ? "ok" : "MISMATCH"); CHECK(ok);

        tex_t *all[] = {&rt, &srgb, &ui, &arr, &dep, &sten, &pm};
        for (unsigned i = 0; i < sizeof all / sizeof *all; i++) mtl_tex_release(all[i]);
    }

    // Release: the image goes away only once the GPU is done with it
    for (int i = 0; i < 6; i++) mtl_tex_release(&td[i].t);
    list_begin(); list_run();
    mtl_drain();
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
