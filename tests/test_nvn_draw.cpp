// Draws from guest code through the NVN API on the Vulkan backend: tests/guest/nvn_draw.c records six stages of draws, each checking one behaviour
// (default state, culling, blending, depth, channel mask, uniform buffers) against pixels the GPU produced. The shaders are registered by hand as SPIR-V:
// the translated library that normally supplies them is not loaded yet. With a fourth argument, a shader library built from Maxwell machine code by the translator
// (tests/make_maxwell_lib.py) supplies stages nine to fifteen's fragment shader.
extern "C" {
#include "../src/host/guest_mem.h"
#include "../src/host/module.h"
#include "../src/host/hle_stub.h"
#include "../src/host/kernel.h"
#include "../src/host/sha256.h"
#include "../src/gfx/nvn/nvn_shader_vk.h"
#include "../src/gfx/nvn/nvn_vk.h"
void hle_collect_sections(void);
}
#include <elf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)
#define EXPECT(i, v) do { if (res[i] != (uint64_t)(v)) { printf("FAIL g_res[%d] = %#llx, expected %#llx\n", i, (unsigned long long)res[i], (unsigned long long)(v)); g_fail = 1; } } while (0)

static std::vector<uint8_t> slurp(const char *path) {
    FILE *f = fopen(path, "rb"); std::vector<uint8_t> d;
    if (!f) { perror(path); return d; }
    fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

// The key hle_nvn.c keys_hash makes: SHA-256 of the control section (0x880 bytes here) followed by the code, first 8 bytes
static void shader_key(const uint8_t *control, const uint8_t *code, uint32_t ncode, uint8_t out[8]) {
    sha256_ctx c; uint8_t md[32];
    sha256_init(&c); sha256_update(&c, control, 0x880); sha256_update(&c, code, ncode); sha256_final(&c, md);
    memcpy(out, md, 8);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s nvn_draw.so draw.vert.spv draw.frag.spv [maxwell.nxsl]\n", argv[0]); return 2; }
    if (guest_mem_init(512ull << 20)) return 1;
    kernel_init();
    hle_collect_sections();

    auto file = slurp(argv[1]);
    CHECK(!file.empty());
    module_t m;
    if (const char *bss = getenv("NX_IMG_BSS")) {
        // A memory image made by tools/nso.py from the ExeFS main of a (synthetic) title, bss as a sparse tail of NX_IMG_BSS bytes: the loader's own path, MOD0 and all
        size_t nbss = strtoull(bss, 0, 0);
        CHECK(file.size() > nbss);
        size_t len = file.size() - nbss;
        int64_t dyn = module_dyn_from_mod0(file.data(), len);
        CHECK(dyn > 0);
        CHECK(module_map(&m, "main", file.data(), len, nbss, (uint64_t)dyn) == 0);
    } else {
        auto *eh = (Elf64_Ehdr *)file.data(); auto *ph = (Elf64_Phdr *)(file.data() + eh->e_phoff);
        uint64_t end = 0, dyn = 0;
        for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end) end = ph[i].p_vaddr + ph[i].p_memsz;
        std::vector<uint8_t> img(end);
        for (int i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type == PT_LOAD) memcpy(img.data() + ph[i].p_vaddr, file.data() + ph[i].p_offset, ph[i].p_filesz);
            if (ph[i].p_type == PT_DYNAMIC) dyn = ph[i].p_vaddr;
        }
        CHECK(module_map(&m, "nvn_draw", img.data(), img.size(), 0x1000, dyn) == 0);
    }
    module_register(&m);
    module_t *loaded[] = {&m};
    CHECK(module_relocate(&m, loaded, 0) == 0);

    // The program's two shaders: a control section (its own length at +0x20, the code size at +0x704) and 64 bytes of code each. The bytes only have to differ.
    auto *vs_ctl = (uint8_t *)module_sym(&m, "g_vs_control"), *fs_ctl = (uint8_t *)module_sym(&m, "g_fs_control"), *pool = (uint8_t *)module_sym(&m, "g_poolmem");
    CHECK(vs_ctl && fs_ctl && pool);
    auto *x_ctl = (uint8_t *)module_sym(&m, "g_x_control");
    auto *t_ctl = (uint8_t *)module_sym(&m, "g_t_control");
    memset(vs_ctl, 0x11, 0x880); memset(fs_ctl, 0x22, 0x880); memset(x_ctl, 0x33, 0x880); memset(t_ctl, 0x44, 0x880);
    auto *v2_ctl = (uint8_t *)module_sym(&m, "g_v2_control"), *p2_ctl = (uint8_t *)module_sym(&m, "g_p2_control");
    auto *v3_ctl = (uint8_t *)module_sym(&m, "g_v3_control"), *p3_ctl = (uint8_t *)module_sym(&m, "g_p3_control"), *p4_ctl = (uint8_t *)module_sym(&m, "g_p4_control"), *p5_ctl = (uint8_t *)module_sym(&m, "g_p5_control");
    memset(v2_ctl, 0x55, 0x880); memset(p2_ctl, 0x66, 0x880); memset(v3_ctl, 0x77, 0x880); memset(p3_ctl, 0x88, 0x880); memset(p4_ctl, 0x99, 0x880); memset(p5_ctl, 0xAA, 0x880);
    for (uint8_t *c : {vs_ctl, fs_ctl, x_ctl, t_ctl, v2_ctl, p2_ctl, v3_ctl, p3_ctl, p4_ctl, p5_ctl}) { uint32_t own = 0x880, ncode = 64; memcpy(c + 0x20, &own, 4); memcpy(c + 0x704, &ncode, 4); }
    memset(pool + 0x0, 0xA1, 64); memset(pool + 0x100, 0xB2, 64); memset(pool + 0x200, 0xC3, 64); memset(pool + 0x300, 0xD4, 64); memset(pool + 0x400, 0xE5, 64); memset(pool + 0x500, 0xF6, 64); memset(pool + 0x600, 0x07, 64); memset(pool + 0x700, 0x18, 64); memset(pool + 0x800, 0x29, 64); memset(pool + 0x900, 0x3A, 64); memset(pool + 0xA00, 0x4B, 64);   // the last two shaders are never registered
    auto vs_spv = slurp(argv[2]), fs_spv = slurp(argv[3]);
    CHECK(!vs_spv.empty() && !fs_spv.empty());
    uint8_t kv[8], kf[8];
    shader_key(vs_ctl, pool + 0x0, 64, kv); shader_key(fs_ctl, pool + 0x100, 64, kf);
    const nvn_shbind vb[] = {{0, NVN_SB_CBUF, 0}}, fb[] = {{1, NVN_SB_CBUF, 0}};
    nvn_shader_desc vd = {NVN_STAGE_VERTEX, 0, (const uint32_t *)vs_spv.data(), vs_spv.size(), vb, 1}, fd = {NVN_STAGE_FRAGMENT, 0, (const uint32_t *)fs_spv.data(), fs_spv.size(), fb, 1};
    nvn_vk_shader_register(kv, &vd); nvn_vk_shader_register(kf, &fd);
    bool have_lib = argc > 4;
    if (have_lib) CHECK(nvn_vk_shader_load_library(argv[4]) == 7);

    uint64_t stack = guest_alloc(0x10000, 0x1000);
    kernel_run_main(module_sym(&m, "guest_main"), 0, stack + 0x10000);
    kernel_wait_all();

    auto *res = (uint64_t *)module_sym(&m, "g_res");
    printf("on %s: %llu draws, %llu skipped, %llu pipelines\n", nvn_gpu_name(), (unsigned long long)nvn_draw_count(), (unsigned long long)nvn_draw_skipped(), (unsigned long long)nvn_pipeline_count());
    printf("blend pixel %08llx (bytes R G B A: want 80 80 00 bf), depth bits at the triangle %08llx (0.25 = 3e800000) and at a corner %08llx (1.0 = 3f800000)\n",
           (unsigned long long)res[20], (unsigned long long)res[21], (unsigned long long)res[22]);
    EXPECT(0, 1);        // the setup chain: device, pool, queue, command buffer, two targets, the program
    EXPECT(1, 1);        // A  a CCW triangle in the default state, the clear around it
    EXPECT(2, 1);        // B  cull back: the CW triangle is not drawn
    EXPECT(3, 1);        // B  cull front: it is, and the CCW one is not
    EXPECT(4, 1);        // C  blending: half red, half green
    EXPECT(5, 1);        // D  depth test: the nearer triangle stays
    EXPECT(6, 1);        // D  and the stored depth is 0.25: depth_clip_control gives GL's -w..w clip volume
    EXPECT(7, 1);        // D  the rest of the depth buffer kept its clear
    EXPECT(8, 1);        // E  channel mask: only red written
    EXPECT(9, 1);        // F  uniform buffers: shifted by the vertex one, halved by the fragment one
    EXPECT(11, 1);       // G  an indexed draw: the indices pick the vertices
    EXPECT(12, 1);       // G  the same indices backwards is the mirror image, which the culling removes: the order is the index buffer's
    EXPECT(13, 1);       // H  a program with no registered shaders: the draw is skipped and the frame goes on
    if (have_lib) { EXPECT(14, 1); EXPECT(15, 1); EXPECT(16, 1); EXPECT(17, 1); EXPECT(18, 1); EXPECT(19, 1); EXPECT(24, 1); }   // I  a fragment shader translated from Maxwell machine code: constant orange; J  a translated vertex/fragment pair; K  one that reads a uniform buffer; L  one that samples a texture; M  one that reads the driver segments c0 and c2 and the support buffer; N  an unregistered texture reads zeros; O  a storage buffer
    EXPECT(25, 1);       // P  a window was made and a frame presented
    {
        // what the presenter received: the stage's frame, the green triangle on blue, as RGBA8
        uint32_t pw = 0, ph = 0; std::vector<uint8_t> fr(64 * 64 * 4);
        CHECK(nvn_present_count() == 1);
        CHECK(nvn_present_last(&pw, &ph, fr.data(), fr.size()) && pw == 64 && ph == 64);
        auto at = [&](int x, int y, int r, int g, int b) { const uint8_t *p = &fr[(y * 64 + x) * 4]; return p[0] == r && p[1] == g && p[2] == b && p[3] == 255; };
        CHECK(at(17, 23, 0, 255, 0) && at(50, 32, 0, 0, 255));
    }
    EXPECT(10, 0x5555);
    CHECK(nvn_draw_count() == (have_lib ? 21u : 14u) && nvn_draw_skipped() == (have_lib ? 1u : 8u));
    CHECK(nvn_pipeline_count() >= 5);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
