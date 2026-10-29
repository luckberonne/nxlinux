// The nxlinux executable: maps the modules of a title (tools/run_title.py unpacks them from the ExeFS into memory images and a manifest), relocates them against the host's
// service hooks, and runs the first guest thread. Usage:
//   nxlinux --manifest FILE [--entry SYMBOL] [--shaders LIB.nxsl] [--mem-mb N]
// The manifest has one line per module in load order: "name image-path bss-bytes". Retail titles start at offset 0 of the first module (rtld); --entry names a symbol instead
// (the synthetic test titles). A window opens by default (NX_PRESENT=sdl); set NX_PRESENT=memory for none.
#include "dm.h"
#include "hle.h"
#include "guest_mem.h"
#include "module.h"
#include "kernel.h"
#include "../gfx/nvn/nvn_shader_vk.h"
#include <stdlib.h>
#include <string.h>

enum { MAXMOD = 32 };

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("[RED] cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(len ? (size_t)len : 1);
    if (fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return NULL; }
    fclose(f); *n = (size_t)len;
    return d;
}

int main(int argc, char **argv) {
    const char *manifest = NULL, *entry_sym = NULL, *shaders = NULL; unsigned mem_mb = 4096;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry_sym = argv[++i];
        else if (!strcmp(argv[i], "--shaders") && i + 1 < argc) shaders = argv[++i];
        else if (!strcmp(argv[i], "--mem-mb") && i + 1 < argc) mem_mb = (unsigned)atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s --manifest FILE [--entry SYMBOL] [--shaders LIB.nxsl] [--mem-mb N]\n", argv[0]); return 2; }
    }
    if (!manifest) { fprintf(stderr, "nxlinux: --manifest is required\n"); return 2; }
    if (!getenv("NX_PRESENT")) setenv("NX_PRESENT", "sdl", 1);
    g_t0_ns = now_ns();
    if (guest_mem_init((uint64_t)mem_mb << 20)) { LOG("[RED] cannot reserve %u MiB of guest memory", mem_mb); return 1; }
    kernel_init();
    hle_collect_sections();

    FILE *mf = fopen(manifest, "r");
    if (!mf) { LOG("[RED] cannot open the manifest %s", manifest); return 1; }
    static module_t mods[MAXMOD]; module_t *loaded[MAXMOD]; int nm = 0;
    char line[2048];
    while (fgets(line, sizeof line, mf) && nm < MAXMOD) {
        char name[64], path[1900]; unsigned long long bss;
        if (sscanf(line, "%63s %1899s %llx", name, path, &bss) != 3) continue;
        size_t total; uint8_t *file = slurp(path, &total);
        if (!file || total <= bss) { LOG("[RED] module %s: image %s is unusable", name, path); return 1; }
        size_t len = total - (size_t)bss;
        int64_t dyn = module_dyn_from_mod0(file, len);
        if (dyn <= 0) { LOG("[RED] module %s has no MOD0 block", name); return 1; }
        if (module_map(&mods[nm], name, file, len, (size_t)bss, (uint64_t)dyn)) { LOG("[RED] module %s could not be mapped", name); return 1; }
        module_register(&mods[nm]); loaded[nm] = &mods[nm];
        LOG("module %-10s base %#llx size %#zx", name, (unsigned long long)mods[nm].base, mods[nm].size);
        free(file); nm++;
    }
    fclose(mf);
    if (!nm) { LOG("[RED] the manifest lists no modules"); return 1; }
    int unimpl = 0;
    for (int i = 0; i < nm; i++) unimpl += module_relocate(&mods[i], loaded, nm);
    if (unimpl) LOG("%d import(s) have no host implementation yet (each is reported when called)", unimpl);
    if (shaders) { int n = nvn_vk_shader_load_library(shaders); LOG("shader library %s: %d shader(s)", shaders, n); }

    uint64_t entry = entry_sym ? 0 : mods[0].base;
    if (entry_sym) for (int i = 0; i < nm && !entry; i++) entry = module_sym(&mods[i], entry_sym);
    if (!entry) { LOG("[RED] no entry point (%s)", entry_sym ? entry_sym : "first module"); return 1; }
    uint64_t stack = guest_alloc(0x100000, 0x1000);
    LOG("starting at %#llx", (unsigned long long)entry);
    kernel_run_main(entry, 0, stack + 0x100000);
    kernel_wait_all();
    LOG("the main thread has exited");
    return 0;
}
