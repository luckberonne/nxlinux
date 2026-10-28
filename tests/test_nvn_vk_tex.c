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

    // Release: the image goes away only once the GPU is done with it
    for (int i = 0; i < 6; i++) mtl_tex_release(&td[i].t);
    list_begin(); list_run();
    mtl_drain();
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
