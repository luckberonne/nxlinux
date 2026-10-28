// Runs tests/guest/memory.c (aarch64, built with clang) against the memory SVCs.
extern "C" {
#include "../src/host/guest_mem.h"
#include "../src/host/module.h"
#include "../src/host/hle_stub.h"
#include "../src/host/kernel.h"
#include "../src/host/mem.h"
}
#include <elf.h>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)
#define EXPECT(i, v) do { if (res[i] != (uint64_t)(v)) { printf("FAIL g_res[%d] = %#llx, expected %#llx\n", i, (unsigned long long)res[i], (unsigned long long)(v)); g_fail = 1; } } while (0)

static int g_breaks; static uint64_t g_break_reason;
static void on_break(uint64_t reason, uint64_t, uint64_t, uint64_t) { g_breaks++; g_break_reason = reason; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    mem_profile_t p = mem_default_profile();
    p.low = 0x10000000; p.alias = 0x1000000; p.stack = 0x1000000; p.heap = 0x8000000; p.app_pool = 0x4000000; p.mem_used = 0x400000;
    if (guest_mem_init(mem_window_size(&p))) return 1;
    CHECK(mem_init(&p) == 0);
    mem_set_break_handler(on_break);
    kernel_init();

    FILE *f = fopen(argv[1], "rb"); std::vector<uint8_t> file;
    fseek(f, 0, SEEK_END); file.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    CHECK(fread(file.data(), 1, file.size(), f) == file.size()); fclose(f);
    auto *eh = (Elf64_Ehdr *)file.data(); auto *ph = (Elf64_Phdr *)(file.data() + eh->e_phoff);
    uint64_t end = 0, dyn = 0, ro = 0, data = 0;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end) end = ph[i].p_vaddr + ph[i].p_memsz;
    std::vector<uint8_t> img(end);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            memcpy(img.data() + ph[i].p_vaddr, file.data() + ph[i].p_offset, ph[i].p_filesz);
            if (!(ph[i].p_flags & PF_X) && !(ph[i].p_flags & PF_W) && !ro) ro = ph[i].p_vaddr;      // first read-only segment
            if ((ph[i].p_flags & PF_W) && !data) data = ph[i].p_vaddr;                              // first writable segment
        }
        if (ph[i].p_type == PT_DYNAMIC) dyn = ph[i].p_vaddr;
    }
    module_t m;
    CHECK(module_map(&m, "memory", img.data(), img.size(), 0x1000, dyn) == 0);
    m.ro_off = ro; m.data_off = data;
    module_register(&m);
    module_t *loaded[] = {&m};
    CHECK(module_relocate(&m, loaded, 0) == 0);

    uint64_t stack = guest_alloc(0x10000, 0x1000);
    mem_register_stack(stack, 0x10000);
    kernel_run_main(module_sym(&m, "guest_main"), 0, stack + 0x10000);
    kernel_wait_all();

    auto *res = (uint64_t *)module_sym(&m, "g_res");
    auto *info = (uint64_t *)module_sym(&m, "g_info");
    CHECK(info[0] == mem_heap_base() && info[1] == p.heap);
    CHECK(info[2] == mem_alias_base() && info[3] == p.alias);
    CHECK(info[4] == mem_stack_base() && info[5] == p.stack);
    CHECK(info[6] == p.app_pool);
    EXPECT(0, 0); EXPECT(1, 1); EXPECT(2, 0); EXPECT(3, 1);
    EXPECT(4, 0xD001); EXPECT(5, 0x10801); EXPECT(6, 0); EXPECT(7, 0); EXPECT(8, 1); EXPECT(9, 0);
    EXPECT(10, 0); EXPECT(11, 1); EXPECT(12, 1); EXPECT(13, 1);
    EXPECT(14, 0xD401); EXPECT(15, 0xD401); EXPECT(16, 0xCC01); EXPECT(17, 0xCA01); EXPECT(18, 0xDC01); EXPECT(19, 0xD401);
    EXPECT(20, 0); EXPECT(21, 1); EXPECT(22, 0);
    EXPECT(23, 0); EXPECT(24, 1); EXPECT(25, 1); EXPECT(26, 0); EXPECT(27, 0x77); EXPECT(28, 0); EXPECT(29, 1);
    EXPECT(30, 0); EXPECT(31, 0); EXPECT(32, 0xDC01); EXPECT(33, 0xDC01);
    EXPECT(34, 1); EXPECT(35, 1); EXPECT(36, 1); EXPECT(37, 1); EXPECT(38, 1); EXPECT(39, 1); EXPECT(40, 0x5555);
    CHECK(g_breaks == 1 && g_break_reason == 0x80000000ull);
    printf("heap=%#llx alias=%#llx stack=%#llx, %d break notification(s)\n",
           (unsigned long long)info[0], (unsigned long long)info[2], (unsigned long long)info[4], g_breaks);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
