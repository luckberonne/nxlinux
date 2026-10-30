// The nxlinux executable: maps the modules of a title (tools/run_title.py unpacks them from the ExeFS into memory images and a manifest), relocates them against the host's
// service hooks, and runs the first guest thread. Usage:
//   nxlinux --manifest FILE [--entry SYMBOL] [--shaders LIB.nxsl] [--mem-mb N] [--arch32]
// The manifest has one line per module in load order: "name image-path bss-bytes [ro-offset data-offset]" (hex; the offsets split the image into code, read-only and data for
// QueryMemory). --arch32 runs a 32-bit (AArch32) process, as main.npdm declares: the window sits in the low 4 GiB and the modules are ELF32. Retail titles start at offset 0 of the first module (rtld); --entry names a symbol instead
// (the synthetic test titles). A window opens by default (NX_PRESENT=sdl); set NX_PRESENT=memory for none.
#include "dm.h"
#include "hle.h"
#include "guest_mem.h"
#include "module.h"
#include "module32.h"
#include "cpu32.h"
#include "ipc.h"
#include "kernel.h"
#include "mem.h"
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
    const char *manifest = NULL, *entry_sym = NULL, *shaders = NULL; unsigned mem_mb = 4096; int arch32 = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry_sym = argv[++i];
        else if (!strcmp(argv[i], "--shaders") && i + 1 < argc) shaders = argv[++i];
        else if (!strcmp(argv[i], "--arch32")) arch32 = 1;
        else if (!strcmp(argv[i], "--romfs") && i + 1 < argc) fsp_set_romfs(argv[++i]);
        else if (!strcmp(argv[i], "--aoc") && i + 1 < argc) { if (aoc_add(argv[++i])) return 2; }
        else if (!strcmp(argv[i], "--mem-mb") && i + 1 < argc) mem_mb = (unsigned)atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s --manifest FILE [--entry SYMBOL] [--shaders LIB.nxsl] [--mem-mb N] [--arch32] [--romfs IMAGE] [--aoc ID:IMAGE]...\n", argv[0]); return 2; }
    }
    if (!manifest) { fprintf(stderr, "nxlinux: --manifest is required\n"); return 2; }
    if (!getenv("NX_PRESENT")) setenv("NX_PRESENT", "sdl", 1);
    g_t0_ns = now_ns();
    if (arch32) cpu_set_aarch32(1);
    if (arch32 ? guest_mem_init32((uint64_t)mem_mb << 20) : guest_mem_init((uint64_t)mem_mb << 20)) { LOG("[RED] cannot reserve %u MiB of guest memory", mem_mb); return 1; }
    if (!entry_sym) { mem_profile_t mp = mem_default_profile();
        if (arch32) { mp.heap = 0x80000000; mp.app_pool = 0xC0000000; }      // a 32-bit title asks for its heap in one piece (Mario Kart 8 Deluxe: SetMemoryHeapSize(0x78000000))
        else { mp.heap = 0xD0000000; mp.app_pool = 0xCD500000; mp.stack = 0x10000000; }      // a 64-bit retail title (Mario Kart 8 Deluxe 4.x: SetHeapSize(0xc1c00000)) asks for 3 GB of heap
        if (mem_init(&mp)) return 1; }   // a retail title brings its own heap and stack: SetHeapSize, MapMemory, QueryMemory
    if (!arch32) kernel_init();            // an AArch32 title's rtld must be the first thing in the window: kernel_init allocates after the modules are mapped
    hle_collect_sections();

    FILE *mf = fopen(manifest, "r");
    if (!mf) { LOG("[RED] cannot open the manifest %s", manifest); return 1; }
    static module_t mods[MAXMOD]; module_t *loaded[MAXMOD]; static module32_t mods32[MAXMOD]; module32_t *loaded32[MAXMOD]; int nm = 0;
    char line[2048];
    while (fgets(line, sizeof line, mf) && nm < MAXMOD) {
        char name[64], path[1900]; unsigned long long bss, ro = 0, data = 0;
        if (sscanf(line, "%63s %1899s %llx %llx %llx", name, path, &bss, &ro, &data) < 3) continue;
        size_t total; uint8_t *file = slurp(path, &total);
        if (!file || total <= bss) { LOG("[RED] module %s: image %s is unusable", name, path); return 1; }
        size_t len = total - (size_t)bss;
        int64_t dyn = module_dyn_from_mod0(file, len);
        if (dyn <= 0) { LOG("[RED] module %s has no MOD0 block", name); return 1; }
        if (arch32 ? module32_map(&mods32[nm], &mods[nm], name, file, len, (size_t)bss, (uint64_t)dyn, ro, data)
                   : module_map(&mods[nm], name, file, len, (size_t)bss, (uint64_t)dyn)) { LOG("[RED] module %s could not be mapped", name); return 1; }
        module_register(&mods[nm]); loaded[nm] = &mods[nm]; loaded32[nm] = &mods32[nm];
        LOG("module %-10s base %#llx size %#zx", name, (unsigned long long)mods[nm].base, mods[nm].size);
        free(file); nm++;
    }
    fclose(mf);
    if (arch32) kernel_init();
    if (!nm) { LOG("[RED] the manifest lists no modules"); return 1; }
    int unimpl = 0;
    // A retail AArch32 title is relocated by its own rtld, which does it for itself and for every module (REL relocations add the base, so doing it here as well would apply them twice).
    // The host relocates only what it starts directly (--entry, the synthetic titles).
    for (int i = 0; i < nm && entry_sym; i++) {
        int n = arch32 ? module32_relocate(&mods32[i], loaded32, nm) : module_relocate(&mods[i], loaded, nm);
        if (n < 0) { LOG("[RED] module %s could not be relocated", mods[i].name); return 1; }
        unimpl += n;
    }
    if (!entry_sym) {            // the host's functions replace the guest's by name; rtld, which relocates the modules, then finds them in place
        int taken = 0, skipped = 0;
        for (int i = 0; i < nm; i++) { int sk = 0; taken += arch32 ? module32_hook_exports(&mods32[i], &sk) : module_hook_exports(&mods[i], &sk); skipped += sk; }
        LOG("hooks: %d guest function(s) taken over by the host%s", taken, skipped ? ", some skipped (see above)" : "");
        if (!arch32 && getenv("NX_TRAP")) {            // NX_TRAP=<hex>,...: absolute guest addresses of `mov xd, xm` instructions
            for (const char *c = getenv("NX_TRAP"); *c; ) { char *e; uint64_t a = strtoull(c, &e, 16); cpu_set_trap64(a); c = *e == ',' ? e + 1 : e; if (!*e) break; }
        }
        if (arch32 && getenv("NX_TRAP")) { const char *tp = getenv("NX_TRAP");            // NX_TRAP=<hex offset in main>,...: log the registers there (the instruction must be a `mov rd, rm`)
            for (int i = 0; i < nm; i++) if (!strcmp(mods[i].name, "main")) for (const char *c = tp; *c; ) {
                int abs_ = *c == 'a'; if (abs_) c++;            // "a<hex>": an absolute guest address (another module), else an offset in main
                char *e; uint32_t off = (uint32_t)strtoul(c, &e, 16); cpu32_set_trap(abs_ ? off : (uint32_t)(mods[i].base + off));
                c = *e == ',' ? e + 1 : e; if (!*e) break;
            }
        }
        if (arch32 && getenv("NX_PATCH_RET")) { const char *pr = getenv("NX_PATCH_RET");            // NX_PATCH_RET=<hex offset in main>,...: those ARM functions return 0 at once (an experiment to skip a part of the title)
            for (int i = 0; i < nm; i++) if (!strcmp(mods[i].name, "main")) for (const char *c = pr; *c; ) {
                char *e; uint32_t off = (uint32_t)strtoul(c, &e, 16);
                uint32_t *fn = (uint32_t *)(uintptr_t)(mods[i].base + off); fn[0] = 0xE3A00000; fn[1] = 0xE12FFF1E;   // mov r0, #0 ; bx lr
                LOG("patched main+%#x to return 0", off);
                c = *e == ',' ? e + 1 : e; if (e == c && !*e) break;
            }
        }
    }
    if (unimpl) LOG("%d import(s) have no host implementation yet (each is reported when called)", unimpl);
    if (shaders) { int n = nvn_vk_shader_load_library(shaders); LOG("shader library %s: %d shader(s)", shaders, n); }

    uint64_t entry = entry_sym ? 0 : mods[0].base;
    if (entry_sym && entry_sym[0] == '@') { for (int i = 0; i < nm; i++) if (!strcmp(mods[i].name, entry_sym + 1)) entry = mods[i].base; }      // --entry @main: the start of that module (the host relocated the modules and rtld is skipped)
    else if (entry_sym) for (int i = 0; i < nm && !entry; i++) entry = arch32 ? module32_sym(&mods32[i], entry_sym) : module_sym(&mods[i], entry_sym);
    if (!entry) { LOG("[RED] no entry point (%s)", entry_sym ? entry_sym : "first module"); return 1; }
    uint64_t stack = guest_alloc(0x100000, 0x1000);
    if (!entry_sym) mem_register_stack(stack, 0x100000);
    LOG("starting at %#llx", (unsigned long long)entry);
    kernel_run_main(entry, 0, stack + 0x100000);
    kernel_wait_all();
    LOG("the main thread has exited");
    return 0;
}
