# Mario Kart 8 Deluxe on nxlinux: bring-up notes

State on 2026-10-30: the 4.0.1 build (a 64-bit process) boots, runs its preload, renders with the translated shader library and shows its title
splash (the key art with the logo) in the SDL window at 60 Hz. Pressing A passes the title and reaches the player-select menu (input is done, see
[Controls](#controls)). Since the shaders' constant segment c[1] is bound, the menu text and the intro screens are drawn; the glyphs come out as
horizontal slices (the font sheets themselves de-swizzle correctly; the text draw's per-character data is next to look at) and the menu background is black. Not done: audio, races (never reached), the movie decoder (`nvdrv` command 13: its thread faults,
the rest goes on), texture writes by the CPU after a texture's first use. The 1.0.0 base alone is a 32-bit process and
boots too, but it has not been taken as far (see [The 32-bit base](#the-32-bit-base-1-0-0)).

Keys, dumps and firmware never go in the repo: `work/` and `*.keys`, `*.nsp`, `*.nsz` are git-ignored. The reference implementation this
ports is `seedds/nxnative`; its Mario Kart profile (`dm/games/0100152000022000/`) is the best description of what the game needs.

## Movies

The menus play `content:/UI/cmn/movie/MovieMenu.mp4` (H.264, 512x608) behind them and wait for its frames: with the SDK's hardware decoder (NVIDIA's NVOMX over nvdrv) the decoder thread
dies and the menus never move past the first choice. `src/host/svc/hle_movie.c` takes over `movie::Decoder` (its methods forward to a `DecoderImpl` the host never makes)
and decodes with FFmpeg (libavcodec, found by pkg-config; without it the frames are black). The SDK's own extractor still reads the MP4. The SPS and PPS come from the
track's `avcc` record, the frames go out as NV12 into the title's buffers, and input is held back while the output slots are full.
With it, A goes through single player, Grand Prix, 50cc and into character select.

## Controls

The window is player 1's Pro Controller, always connected (`src/host/svc/hid_input.c` fills the `hid` shared memory: the npad entry of player 1 at +0x9A00, style set Pro Controller, a ring of 17 states, a new one every 8 ms; the SDL thread in `nvn_present_vk.c` feeds it).
Keyboard: arrows = D-pad, WASD = left stick, K or Return = A, J or Backspace = B, I = X, U = Y, Q = L, E = R, 1 = ZL, 3 = ZR, P = Plus, O = Minus.
A gamepad SDL knows works too, with its physical layout (the east button is A). `hid` commands 100/101 keep the style set the title asks for; 11, 66, 67, 120 to 128 are accepted; 203 returns an empty vibration device list.

## Running it

```
python3 -m venv .venv && .venv/bin/pip install zstandard cryptography lz4      # once
cmake -S . -B build-NAME -G Ninja && cmake --build build-NAME

# the update ships as .nsz (zstd-compressed NCAs): turn it into a plain NSP
.venv/bin/python tools/nsz_to_nsp.py "update.nsz" work/run/mk8d/upd/update.nsp

# extract base + update, unpack the modules, build one RomFS image (about 27 s; the RomFS is 7 GB: never extract it under /tmp, which is RAM)
.venv/bin/python tools/run_title.py --keys work/keys/prod.keys --nsp "base.nsp" --update work/run/mk8d/upd/update.nsp \
    --work work/run/mk8d_u --exe build-NAME/nxlinux --romfs-image --no-run

# capture the shaders the game uses while it runs, translate them, and start the game with the library
NX_SHADER_DUMP=work/run/mk8d_u/shd NX_NVN_API=53.313 NX_PRESENT=null build-NAME/nxlinux --manifest work/run/mk8d_u/modules.txt \
    --romfs work/run/mk8d_u/extract/RomFS.img          # about 70 s, then stop it
.venv/bin/python tools/shader_capture_build.py work/run/mk8d_u/shd work/run/mk8d_u/mk8d.nxsl
tools/run_mk8d.sh build-NAME                           # window; NX_PRESENT=null for no window
```

`tools/run_mk8d.sh` loads `work/run/mk8d_u/mk8d.nxsl` when it exists and sets `NX_NVN_API=53.313`. The shader library is a cache of what a run
captured: a shader the library lacks makes its draws skip (`[RED] ... not in the registry`), so after playing further, dump and rebuild again.
The device must report NVN API 53.x with minor >= 105: the game compares it at `main+0xa83f24` and skips creating its graphics object otherwise.

## What a retail title needs from the host

### Process model
- A title is started at offset 0 of `rtld`, as Horizon does. rtld relocates every module itself, so the host must not: `main.c` relocates only
  what it starts directly (`--entry`, the synthetic titles). Instead `module_hook_exports` (A64) / `module32_hook_exports` (A32) overwrite the
  entry of every function a module exports that the host implements (`hle_implements`) with a jump to the host's stub, so every caller reaches it
  whichever way rtld binds it. A function of 8 to 15 bytes gets the stub's own `svc #n; ret`, one of 4 to 7 bytes a `b` to it; short functions are
  taken over only for `nn::mii` validity checks and `nn::oe`, because the host versions of the others were never meant to replace them.
- Modules are placed page-aligned and back to back from the start of the window (rtld walks them with `QueryMemory`).
  `QueryMemory` reports the free space between modules and to the end of the address space as large regions (a page-sized answer sent rtld on a walk
  over 2^39 bytes). The thread-local region and the host's own structures are one `ThreadLocal` region.
- The loader hands the main thread its handle in `x1`/`r1`.
- Memory profile (`main.c`): a 32-bit title gets a 2 GiB heap (`SetMemoryHeapSize(0x78000000)`), a 64-bit one 0xD0000000 with a 0xCD500000 pool and a
  256 MB stack region, as nxnative's profile has it.
- Services run at the IPC level (`ipc.c`, `svc/ipc_*.c`): the SDK's own HIPC client talks to the host through `SendSyncRequest`. Domains, control
  messages, A/B/X/C buffer descriptors (36-bit addresses for a 64-bit process), copy and move handles and `QueryPointerBufferSize` are handled.
  Services: sm, lm, apm, aoc:u, pctl, appletOE, fsp-srv (RomFS image, save data under `work/save/<title>`, system data), nvdrv (stub), hid
  (npad shared memory refilled at 120 Hz), set, mii, prepo, pl:u (the host's TrueType font in the shared-font layout), audren:u (SDK 4 and the 1.0.0 formats).
- `cpu_call32` / `cpu_call64` call a guest function from the host on the calling guest thread (used to bind `SystemEvent`s through the SDK's own
  `nn::os::AttachSystemEvent`).
- A per-core priority scheduler (`kernel.c`) lets only one thread per core run: without it the SDK's init races (a worker using an object before
  its owner built it) show up. `NX_NO_SCHED=1` turns it off.

### The MiiModel stand-in
The game builds Mii heads from firmware data (`MiiResource:` = system data 0100000000000802) that the host does not have. `svc/mii_standin.c` serves a
RomFS of six files with every part absent (`NXTexture{Low,Mid}{Linear,SRGB}.dat`, `Shape{Mid,High}.dat`; layout in the file header comment, taken from
nxnative). The sizes it declares must not be 0 (they size the model's memory pool). **The root directory must be in the directory hash table**: the
SDK finds a file's parent through it, and without it `nn::fs::OpenFile` returned `PathNotFound`, the game never read the headers, summed garbage,
failed to allocate and created textures in a NULL pool. The game then needs `ParamCharInfoAccessor::IsValid` and `CharInfoAccessor::IsValid` forced to true
(`svc/hle_mii32.c`, taken over by name in both architectures) because it cannot cope with an invalid Mii.

### Shaders (the part that cost the most time)
- The game's shaders are Maxwell code inside its files (BNSH in `.bfres`/`.bfsha`/`.ptcl`/`.szs`, agl's `.sharcb`); the graphics ones are loaded into
  memory by the title and reach NVN through `nvnProgramSetShaders`. A graphics code area starts with the magic `0x12345678` and a 0x30-byte header; its
  SPH is at +0x30 (bits 10..13 of its first word are the stage: 1 vertex, 5 pixel). A code area whose SPH word is 0 is a reserved stage the title has not
  filled yet (it makes thousands before its assets load). **`0x12345678` is not a placeholder marker.**
- `NX_SHADER_DUMP=<dir>` writes each shader once per key (`<key>.code/.c1/.ctl/.stage`). A program's key is taken when its code is there: a stage that
  was reserved at `ProgramSetShaders` gets its key at the first draw that uses it (`nvn_prog_keys`).
- `tools/shader_capture_build.py` strips the 0x30-byte header from graphics code before the translator sees it (compute code is passed as is),
  runs `tools/dmsh_xlate` (Ryujinx's translator, built with `dotnet build -c Release -o work/dmsh_xlate`) and packs the result.
  The translator numbers a stage's resources from 1; one draw binds both stages in one push-descriptor set, so pixel-stage bindings start at 64.
- The game's own GLSLC (`subsdk0`) never runs for this title: its shaders are all precompiled.

### NVN on Vulkan: what Mario Kart 8 exercises
- **Textures the CPU fills after creating them** (no packaged data, no `nvnMemoryPoolFlushMappedRange`): a texture with storage starts dirty
  (`mtl_tex_init`) and is uploaded when first used. Without this every texture is black. Later CPU writes are not detected: nxnative write-protects
  pages for that, and a guest write through dynarmic's fastmem is not caught by the same handler. Open.
- Three-component vertex formats (`RGB32F` and kin) are vertex-only formats: `vertex_fmt` in `nvn_draw_vk.c` maps them, the texture table does not.
  A disabled vertex attribute (format NONE) keeps its location, trailing ones are dropped, and streams no enabled attribute reads need no buffer.
- A title may pass a NULL memory pool to texture and buffer creation when its allocator record is empty. Those objects get a slice of a private
  pool (`fallback_pool`) so the title does not crash; the real cause was always upstream (see the Mii note above).
- Device info: the API version comes from `NX_NVN_API`; items 0x2E..0x31 report the GPU-code range the translator supports.
- Textures of 1920x1080 RGB10A2 are the title's offscreen scene target; the window textures are the swap chain (serials 2, 3, 4).

### The 32-bit base (1.0.0)
The base is an AArch32 process (`main.npdm` flag): `cpu32.cpp` (dynarmic A32 with a CP15 for the thread pointer and the counter), `module32.c` (ELF32 REL/RELA/RELR),
`svc32.c` (the Horizon AArch32 SVC ABI; hooks get their parameter layout from the mangled name, ILP32), NVN signatures generated by
`tools/gen_nvn_sig32.py`, ILP32 conversions in `hle_nvn.c`, `hle_vi32.c`/`hle_oe32.c`/`hle_account32.c`, and an ASTC LDR decoder (`src/gfx/tex/astc.c`) for
GPUs without ASTC. It runs rtld and the SDK, answers about twenty services and loads the game's assets; it is unchanged since the 64-bit update became the
target. Run it with `--arch32` (`run_title.py` detects it) and `NX_NVN_API=52.32`.

## Debugging aids

All are environment variables; nothing costs anything when unset.

| Variable | Does |
|---|---|
| `NX_NVN_API=<maj.min>` | the NVN API version the device reports (53.313 for the update, 52.32 for the base) |
| `NX_PRESENT=null` | run without a window |
| `NX_SHADER_DUMP=<dir>` | write every shader the game binds, once per key (see above) |
| `NX_FRAME_DUMP=<prefix>` | write presented frames 100, 400, 1000, 2500 and every 1500th to `<prefix>.<n>.ppm`, with the count of non-black pixels and statistics of the offscreen target |
| `NX_DRAW_LOG=1` | first draws, per-target draw counts every 4000 draws, the first render-target sets |
| `NX_DRAW_DETAIL=1` | for a few draws: pipeline state, vertex stream and uniform contents, sampled texture (and a PPM of it), texture uploads, viewport |
| `NX_SKIP_LOG=1` | every skipped draw with its reason (1 in 64 after the first 400) |
| `NX_DRAW_SERIAL=<serial>` | details of the draws that sample that texture: attributes, the first vertex words, the bound uniform buffers |
| `NX_TEX_ARRAYS=1` | with `NX_FRAME_DUMP`: list the 2D array textures at frame 1000 and write layer 0 of the BC4 ones (font sheets) as raw blocks |
| `NX_TEX_NO_RECHECK=1` | do not hash texture storage to catch CPU rewrites after the first upload |
| `NX_HID_SCRIPT="<s>:<bit>[:<ms>],..."` | press buttons at given seconds after the hid block is mapped (each logged); e.g. `50:0,65:15` = A at 50 s, down at 65 s |
| `NX_FRAME_EVERY=<n>` | with `NX_FRAME_DUMP`: also every n-th frame |
| `NX_MOVIE_LOG=1` | the movie decoder takeover's calls (`hle_movie.c`) |
| `NX_HID_TAP=<bit>[:<s>]` | press that `NpadButton` bit (0 = A) for 150 ms every `<s>` seconds (default 4), for runs nobody sits at |
| `NX_NO_CULL=1`, `NX_TEX_CLEAN=1`, `NX_NVN_ITEMS=1` | test switches: no culling, textures start clean, log each device-info item queried |
| `NX_TRACE_SVC` | 32-bit: every SVC and hook. 64-bit: `1` = the first 3000 SVCs, `<name part>` = only the hooks whose name contains it, without limit |
| `NX_SVC_ERRORS=1` | each distinct (SVC, non-zero result) once |
| `NX_SVC_STATS=1` | 32-bit: a histogram of SVCs and hooks with callers every 2 s |
| `NX_IPC_TRACE=1` | every IPC request, its first words, the reply buffer addresses and the reply (`NX_IPC_RAW=1` adds the raw message) |
| `NX_THREADS=<s>` | every `<s>` seconds, each guest thread: priority, core, what it waits on, lr (`NX_THREADS_STACK=1` adds a call stack, `NX_THREADS_ALL=1` also for blocked threads) |
| `NX_WATCH=<hex addr>` | stop at each guest write to that word and print who wrote it (both architectures; host-side writes such as an IPC read are not seen) |
| `NX_TRACE_PC=<file>` (+ `NX_TRACE_PC_AFTER=<s>`) | every instruction the JIT translated, written at exit, after `<s>` seconds and on a host SIGSEGV (u32 addresses in 32-bit, u64 in 64-bit) |
| `NX_TRAP=<addresses>` | log registers (and strings they point at) where the listed `mov rd, rm` instruction runs: absolute addresses on 64-bit; on 32-bit offsets in `main`, `a<hex>` for absolute |
| `NX_STACK_AT=<hook name>` / `NX_STACK_DUMP=<addr>` | 32-bit: print the call stack (and 24 words at an address) when that hook runs with a zero second argument |
| `NX_FAULT_DUMP=<frame>` / `NX_FAULT_STACK=1` | extra detail in the guest fault report (64-bit frame saved registers / 32-bit stack) |
| `NX_ZERO_FAULT=1`, `NX_PATCH_RET=<offsets>` | 32-bit experiments: zero pages for stray accesses, make functions return 0 |
| `NX_NO_HOOK=a,b`, `NX_HOOK_LIST=1` | keep those exports as guest code / list the small functions taken over |
| `NX_NO_SCHED=1`, `NX_WATCHDOG=1` | no per-core scheduler / registers of running threads every second |
| `NX_MII_BUILD=0`, `NX_SHARED_FONT=<ttf>`, `NX_SAVE_DIR=<dir>` | Mii database answers, the font served as the shared fonts, the save directory |

A guest access outside the window stops that thread and prints its registers and the frame-pointer chain, in both architectures.

## Methods that paid off

- **Watch a field, find its constructor, read the code that fills it.** `NX_WATCH` on the NULL field named the code that zeroed it; the code that should
  have filled it was an initializer that bails out when `IsValid()` is false. Each of the three NULL-object crashes (Mii pool, loader, singleton) was a
  step upstream that failed quietly.
- **Trap the result of a library call you cannot hook** (`NX_TRAP` on the `mov` after the call): it showed `nn::fs::OpenFile` failing for the Mii files and
  succeeding for all else.
- **Compare with nxnative before assuming.** Its profile said the shaders live in the assets and that the game runs unpatched; both ended long detours.
- **Look at the pixels.** Dumping the offscreen target and the sampled textures to PPM showed that the game was rendering its title (the textures held
  the logo) while the output stayed black, which pointed at texture upload instead of shaders or draws.
