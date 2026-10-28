// Runs tests/guest/nvn_smoke.c (aarch64) on the emulated CPU against hle_nvn.c and the null GPU: the whole path a game takes,
// guest code -> svc stub -> hle_call -> NVN implementation.
extern "C" {
#include "../src/host/guest_mem.h"
#include "../src/host/module.h"
#include "../src/host/hle_stub.h"
#include "../src/host/kernel.h"
#include "../src/gfx/nvn/nvn_vk.h"
void hle_collect_sections(void);
}
#include <elf.h>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)
#define EXPECT(i, v) do { if (res[i] != (uint64_t)(v)) { printf("FAIL g_res[%d] = %#llx, expected %#llx\n", i, (unsigned long long)res[i], (unsigned long long)(v)); g_fail = 1; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (guest_mem_init(512ull << 20)) return 1;
    kernel_init();
    hle_collect_sections();                   // nvnBootstrapLoader and the other HLE entries of the program

    FILE *f = fopen(argv[1], "rb"); std::vector<uint8_t> file;
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); file.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    CHECK(fread(file.data(), 1, file.size(), f) == file.size()); fclose(f);
    auto *eh = (Elf64_Ehdr *)file.data(); auto *ph = (Elf64_Phdr *)(file.data() + eh->e_phoff);
    uint64_t end = 0, dyn = 0;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end) end = ph[i].p_vaddr + ph[i].p_memsz;
    std::vector<uint8_t> img(end);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) memcpy(img.data() + ph[i].p_vaddr, file.data() + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_type == PT_DYNAMIC) dyn = ph[i].p_vaddr;
    }
    module_t m;
    CHECK(module_map(&m, "nvn_smoke", img.data(), img.size(), 0x1000, dyn) == 0);
    module_register(&m);
    module_t *loaded[] = {&m};
    int unimpl = module_relocate(&m, loaded, 0);
    CHECK(unimpl == 0);                      // nvnBootstrapLoader is registered, so nothing binds to a stub with no host function

    uint64_t stack = guest_alloc(0x10000, 0x1000);
    kernel_run_main(module_sym(&m, "guest_main"), 0, stack + 0x10000);
    kernel_wait_all();

    auto *res = (uint64_t *)module_sym(&m, "g_res");
    printf("API %llu.%llu, pool page %llu, queue command memory min %llu\n", (unsigned long long)res[2], (unsigned long long)res[3], (unsigned long long)res[4], (unsigned long long)res[5]);
    EXPECT(0, 1); EXPECT(1, 1);
    CHECK(res[2] > 0 && res[3] > 0);
    EXPECT(4, 65536);                    // the Tegra X1 value in hle_nvn.c g_devinfo[0x3A]
    CHECK(res[5] > 0);
    EXPECT(6, 1); EXPECT(7, 1); EXPECT(8, 1);
    EXPECT(9, 1); EXPECT(10, 1); EXPECT(11, 1); EXPECT(12, 1);
    EXPECT(13, 1); EXPECT(14, 1); EXPECT(15, 1); EXPECT(16, 1);
    EXPECT(17, 1); EXPECT(18, 1);
    EXPECT(19, 1); EXPECT(20, 1); EXPECT(21, 1); EXPECT(22, 1); EXPECT(23, 1);
    EXPECT(24, 42);                                            // the event: written 7, then 35 added
    EXPECT(25, 1); EXPECT(26, 1); EXPECT(27, 1); EXPECT(28, 1); EXPECT(29, 1); EXPECT(30, 1);
    EXPECT(31, 0x5555);
    EXPECT(40, 1); EXPECT(41, 1);                              // a texture: built, storage in place
    EXPECT(42, 1);                                             // written by the CPU (block-linear in the pool), copied out by the GPU: equal
    EXPECT(43, 1);                                             // patched by a GPU buffer->texture copy with a padded row stride, copied out again
    EXPECT(44, 1); EXPECT(45, 1);                              // ReadTexels: the GPU-written region (read back) and an untouched corner
    CHECK(hle_unimplemented_calls == 0);
    printf("replayed on %s: END %llu, COPY_B2B %llu, CLEAR_BUF %llu, UPDATE_UB %llu, SIGNAL_EVENT %llu, REPORT_COUNTER %llu, FENCE_SYNC %llu, BARRIER %llu\n", nvn_gpu_name(),
           (unsigned long long)nvn_replay_ops_named("OP_END"), (unsigned long long)nvn_replay_ops_named("OP_COPY_B2B"), (unsigned long long)nvn_replay_ops_named("OP_CLEAR_BUF"),
           (unsigned long long)nvn_replay_ops_named("OP_UPDATE_UB"), (unsigned long long)nvn_replay_ops_named("OP_SIGNAL_EVENT"), (unsigned long long)nvn_replay_ops_named("OP_REPORT_COUNTER"),
           (unsigned long long)nvn_replay_ops_named("OP_FENCE_SYNC"), (unsigned long long)nvn_replay_ops_named("OP_BARRIER"));
    CHECK(nvn_replay_ops_named("OP_COPY_B2B") >= 3 && nvn_replay_ops_named("OP_CLEAR_BUF") >= 1 && nvn_replay_ops_named("OP_UPDATE_UB") >= 1 && nvn_replay_ops_named("OP_SIGNAL_EVENT") >= 2);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
