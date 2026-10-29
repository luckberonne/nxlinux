// Runs tests/guest/services.c on the emulated CPU against the ported service hooks (src/host/svc): the SDK's nn:: client functions answered by name, and a forced takeover
// that overrides a definition in the guest module itself.
extern "C" {
#include "../src/host/guest_mem.h"
#include "../src/host/module.h"
#include "../src/host/hle_stub.h"
#include "../src/host/kernel.h"
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
    if (guest_mem_init(256ull << 20)) return 1;
    kernel_init();
    hle_collect_sections();
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
    CHECK(module_map(&m, "services", img.data(), img.size(), 0x1000, dyn) == 0);
    module_register(&m);
    module_t *loaded[] = {&m};
    CHECK(module_relocate(&m, loaded, 0) == 0);          // every import is an implemented hook
    uint64_t stack = guest_alloc(0x10000, 0x1000);
    kernel_run_main(module_sym(&m, "guest_main"), 0, stack + 0x10000);
    kernel_wait_all();
    auto *res = (uint64_t *)module_sym(&m, "g_res");
    for (int i = 0; i < 9; i++) EXPECT(i, 1);
    EXPECT(9, 0x5555);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
