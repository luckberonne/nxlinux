// Runs tests/guest/threads.c (aarch64, built with clang) on the kernel: real guest threads on real host threads.
extern "C" {
#include "../src/host/guest_mem.h"
#include "../src/host/module.h"
#include "../src/host/hle_stub.h"
#include "../src/host/kernel.h"
}
#include <elf.h>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static std::vector<uint8_t> load_elf_image(const char *path, uint64_t *dyn_off) {
    FILE *f = fopen(path, "rb"); std::vector<uint8_t> file;
    if (!f) { perror(path); return {}; }
    fseek(f, 0, SEEK_END); file.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    if (fread(file.data(), 1, file.size(), f) != file.size()) { fclose(f); return {}; }
    fclose(f);
    auto *eh = (Elf64_Ehdr *)file.data(); auto *ph = (Elf64_Phdr *)(file.data() + eh->e_phoff);
    uint64_t end = 0;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end) end = ph[i].p_vaddr + ph[i].p_memsz;
    std::vector<uint8_t> img(end);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) memcpy(img.data() + ph[i].p_vaddr, file.data() + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_type == PT_DYNAMIC) *dyn_off = ph[i].p_vaddr;
    }
    return img;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (guest_mem_init(256ull << 20)) return 1;
    kernel_init();
    uint64_t dyn = 0;
    auto img = load_elf_image(argv[1], &dyn);
    CHECK(!img.empty());
    module_t m;
    CHECK(module_map(&m, "threads", img.data(), img.size(), 0x1000, dyn) == 0);
    module_t *loaded[] = {&m};
    CHECK(module_relocate(&m, loaded, 0) == 0);

    uint64_t sp = guest_alloc(0x10000, 0x1000) + 0x10000;
    kernel_run_main(module_sym(&m, "guest_main"), 0, sp);
    kernel_wait_all();

    auto *res = (uint64_t *)module_sym(&m, "g_res");
    printf("counter=%llu (expected %d)\n", (unsigned long long)res[0], 4 * 3000 + 1000);
    CHECK(res[0] == 4 * 3000 + 1000);          // mutual exclusion held across 4 workers and main
    CHECK(res[10] == 0);                        // every worker handle was waited on successfully
    CHECK(res[1] == 0 && res[2] == 1);         // condition variable: consumer woke and saw the flag
    printf("addr waiter: r=%#llx g_addr=%llu\n", (unsigned long long)res[11], (unsigned long long)res[12]);
    printf("addr: wait_sync=%#llx got=%llu\n", (unsigned long long)res[3], (unsigned long long)res[4]);
    CHECK(res[3] == 0 && res[4] == 1);         // WaitForAddress / SignalToAddress
    CHECK(res[5] == R_TIMEOUT);                 // unsignaled event times out
    CHECK(res[6] == 0 && res[7] == 0);         // signalled by another thread, index 0
    CHECK(res[8] == R_TIMEOUT);                 // after ClearEvent
    CHECK(res[9] == 1);                         // main reached the end
    CHECK(kernel_live_threads() == 0);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
