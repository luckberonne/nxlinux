// Loads a real aarch64 shared object (built by clang + lld at build time) into the guest window, relocates it,
// and runs its functions: an import resolved to an HLE stub, data relocations, a floating-point HLE call, and an
// unimplemented import that must log and return 0 instead of crashing.
extern "C" {
#include "../src/host/guest_mem.h"
#include "../src/host/module.h"
#include "../src/host/hle_stub.h"
}
#include <elf.h>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" long hle_add(long a, long b, long c) { return a + b + c; }
extern "C" double hle_scale(double a, float b) { return a * b; }

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static void on_svc(cpu_t *c, uint32_t swi, void *) {
    if (swi == 1) { cpu_halt(c); return; }
    if (!hle_dispatch(c, swi)) { printf("unknown svc %#x\n", swi); cpu_halt(c); }
}
static uint64_t cntpct(void) { return 0; }

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

static uint64_t call(uint64_t fn, uint64_t a0, uint64_t *d0 = nullptr, uint64_t halt = 0) {
    cpu_ops_t ops = {on_svc, cntpct, nullptr};
    cpu_t *c = cpu_create(&ops, nullptr, 0);
    cpu_set_pc(c, fn); cpu_set_sp(c, guest_mem_end() - 0x1000);
    cpu_set_x(c, 0, a0); cpu_set_x(c, 30, halt);
    if (d0) { uint64_t v[2] = {*d0, 0}; cpu_set_v(c, 0, v); }
    int r = cpu_run(c);
    if (r != CPU_HALT_REQUESTED) printf("run returned %d\n", r);
    uint64_t x0 = cpu_get_x(c, 0);
    if (d0) { uint64_t v[2]; cpu_get_v(c, 0, v); *d0 = v[0]; }
    cpu_destroy(c);
    return x0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (guest_mem_init(256ull << 20)) return 1;
    hle_register("hle_add", (void *)hle_add);
    hle_register("hle_scale", (void *)hle_scale);

    uint64_t halt = guest_alloc(16, 16);                  // a landing pad: `svc #1`
    *(uint32_t *)halt = 0xD4000021u;

    uint64_t dyn = 0;
    auto img = load_elf_image(argv[1], &dyn);
    CHECK(!img.empty());
    module_t m;
    CHECK(module_map(&m, "mod_a", img.data(), img.size(), 0x1000, dyn) == 0);
    module_t *loaded[] = {&m};
    int unimpl = module_relocate(&m, loaded, 0);
    printf("relocated, %d import(s) without a host function\n", unimpl);
    CHECK(unimpl == 1);

    uint64_t entry = module_sym(&m, "entry");
    CHECK(entry != 0);
    CHECK(call(entry, 10, nullptr, halt) == 26 + 7);              // (10+1+2)*2 + g_value
    CHECK(call(module_sym(&m, "via_pointer"), 20, nullptr, halt) == 41);
    double a = 1.5; call(module_sym(&m, "entry_fp"), 0, (uint64_t *)&a, halt);
    CHECK(a == 3.0);
    CHECK(call(module_sym(&m, "call_missing"), 9, nullptr, halt) == 5);
    CHECK(hle_unimplemented_calls == 1);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
