# Mario Kart 8 Deluxe on nxlinux: bring-up notes

State on 2026-10-30: the 4.0.1 build (a 64-bit process) boots, runs its preload, renders with the translated shader library and shows its title
splash (the key art with the logo) in the SDL window at 60 Hz. Pressing A passes the title and reaches the player-select menu (input is done, see
[Controls](#controls)). Menu text and backgrounds render correctly (Single Player, Grand Prix, 100cc, ...; the character and kart screens show their icons and
labels). Not done: audio, races (never reached), the movie panels behind some menus (black rectangles), the movie decoder (`nvdrv` command 13: its thread faults,
the rest goes on), texture writes by the CPU after a texture's first use. The 1.0.0 base alone is a 32-bit process and
boots too, but it has not been taken as far (see [The 32-bit base](#the-32-bit-base-1-0-0)).

Keys, dumps and firmware never go in the repo: `work/` and `*.keys`, `*.nsp`, `*.nsz` are git-ignored. The reference implementation this
ports is `seedds/nxnative`; its Mario Kart profile (`dm/games/0100152000022000/`) is the best description of what the game needs.

## Add-on content and version

`--aoc <id>:<RomFS image>` installs an add-on content: `aoc:u` counts and lists it (index = id - (program id + 0x1000)) and `fsp-srv` command 202 serves its RomFS
from memory. The Booster Course Pass (0100152000023001) is a 4 KiB licence (`contents.dat`, `version.txt` "10002"): the courses are in the update. Extract it with
`work/nx_extract/nx_extract work/keys/prod.keys --aoc DLC.nsp work/run/mk8d_dlc/aoc.romfs --romfs-image`; `tools/run_mk8d.sh` passes it when the file is there.
`nn::oe::GetDisplayVersion` answers the `control.nacp` beside the RomFS image (4.0.1 with the update; `NX_DISPLAY_VERSION` overrides), not a fixed 1.0.0.

## The race

With the movie decoder replaced, A goes through single player, Grand Prix, 50cc, Mario, the default kart and the Mushroom Cup into Mario Kart Stadium, and the race runs:
the kart drives, overtakes, collects coins, the anti-gravity section and the stands are drawn. What it took on the NVN side: room for 16384 memory pools (a course
makes more than a thousand), finalized textures kept alive (the scene's HDR target, 1920x1080 R11G11B10F, is finalized right after it is made and used every frame;
other textures alias its memory), colour targets that are slices of a 3D texture (an 8x8x8 colour LUT), and the shaders captured during a race (2839 in the
library). Since then the ground is drawn too (see "Why the ground was black"): the race shows the asphalt, markings, countdown, karts and the stands. Frame time in the race is
about 70 ms (14 fps) on the Radeon 660M and 78 ms on the RTX 2050, which this machine holds at 607 MHz (power cap); it is CPU bound now (replay and the guest's own code).

## Movies

The menus play `content:/UI/cmn/movie/MovieMenu.mp4` (H.264, 512x608) behind them and wait for its frames: with the SDK's hardware decoder (NVIDIA's NVOMX over nvdrv) the decoder thread
dies and the menus never move past the first choice. `src/host/svc/hle_movie.c` takes over `movie::Decoder` (its methods forward to a `DecoderImpl` the host never makes)
and decodes with FFmpeg (libavcodec, found by pkg-config; without it the frames are black). The SDK's own extractor still reads the MP4. The SPS and PPS come from the
track's `avcc` record, the frames go out as NV12 into the title's buffers, and input is held back while the output slots are full.
With it, A goes through single player, Grand Prix, 50cc and into character select.

## Controls

The window is player 1's Pro Controller, always connected (`src/host/svc/hid_input.c` fills the `hid` shared memory: the npad entry of player 1 at +0x9A00, style set Pro Controller, a ring of 17 states, a new one every 8 ms; the SDL thread in `nvn_present_vk.c` feeds it).
Keyboard: arrows = D-pad, WASD = left stick, K or Return = A, J or Backspace = B, I = X, U = Y, Q = L, E = R, 1 = ZL, 3 = ZR, P = Plus, O = Minus.
A gamepad SDL knows works too: its A and B follow the labels SDL gives the face buttons (bottom = A on Xbox and generic pads, right = A with the Nintendo layout); by default (a driving layout) LB and LT are the console's L and ZL (the game's item buttons) and RB and RT its R (hop, drift), `NX_PAD_LAYOUT=pro` only makes RT send ZR, the right stick has the same 6000 dead zone as the left. The table is in the README. `hid` commands 100/101 keep the style set the title asks for; 11, 66, 67, 120 to 128 are accepted; 203 returns an empty vibration device list.

## Audio

`audren:u` renders on the host (`src/host/svc/audren_mix.c`, IPC in `ipc_audren.c`). Each RequestUpdate (revision 4: 96 voices, the final mix of 6 buffers and one
submix of 24, a 5.1 device sink) updates a server copy of every voice; a clock thread renders 240 samples every 5 ms (PCM8/PCM16/float and DSP-ADPCM wave
buffers, linear resampling, channel resource volumes, submix matrices, 5.1 downmixed to stereo), queues them on the SDL3 audio device and fires the frame event.
The play counts in the answer pace the title's streaming: the menu music is mono ADPCM voices at 32 kHz, one per side. Effects run on their mix in processing
order: the auxiliary send (submix buffers 6 to 11: int32 frames into the title's send ring, the channel replaced by its return ring, which the title's own
effect code fills) and the delay (buffers 12 to 15, 800 ms, wet only) are rendered; a reverb is not, its input is cleared. Unused channel resource entries
are zeros with id 0 and must not overwrite resource 0 (that silenced the intro's left side). `NX_AUDIO=0` keeps the device closed, `NX_AUDIO_WAV=file.wav`
also records the output (headless checks), `NX_AUDREN_TRACE=1` prints each new voice: format, rate, mix and the mix buffers its channels feed.

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
  **c[1] is the end of the code area** (control +0x700 = its offset, +0x704 = the area's length), not the 0x100 bytes before the control section: the first
  version of the capture took those, and for this title they are other data (pointers), so every shader that reads c[1] (glyph quads: the corner tables and
  a 0.25 factor, most backgrounds) drew garbage: text as horizontal slices, black menu backgrounds. `shader_capture_build.py` now derives c[1] from `.code` and
  `.ctl`, so dumps made before the fix only need the library rebuilt.
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
- Memory pools: a course load creates well over a thousand. `nvn_vk.c` holds up to 16384 (`MAX_POOLS`), sorted by base so `pool_find` is a
  binary search; the old 1024-entry table filled up (`pool table full`) while a course loaded.
- Device info: the API version comes from `NX_NVN_API`; items 0x2E..0x31 report the GPU-code range the translator supports.
- Textures of 1920x1080 RGB10A2 are the title's offscreen scene target; the window textures are the swap chain (serials 2, 3, 4).

### The 32-bit base (1.0.0)
The base is an AArch32 process (`main.npdm` flag): `cpu32.cpp` (dynarmic A32 with a CP15 for the thread pointer and the counter), `module32.c` (ELF32 REL/RELA/RELR),
`svc32.c` (the Horizon AArch32 SVC ABI; hooks get their parameter layout from the mangled name, ILP32), NVN signatures generated by
`tools/gen_nvn_sig32.py`, ILP32 conversions in `hle_nvn.c`, `hle_vi32.c`/`hle_oe32.c`/`hle_account32.c`, and an ASTC LDR decoder (`src/gfx/tex/astc.c`) for
GPUs without ASTC. It runs rtld and the SDK, answers about twenty services and loads the game's assets; it is unchanged since the 64-bit update became the
target. Run it with `--arch32` (`run_title.py` detects it) and `NX_NVN_API=52.32`.

## The shader library

The host looks every draw's shaders up in a library of translated SPIR-V (`--shaders`; `tools/run_mk8d.sh` loads `work/run/mk8d_u/mk8d.nxsl`). Capturing shaders while the game runs
never converges (every run reaches programs the earlier ones did not), so the library is built from every shader in the game files, the way nxnative does it:

```
nx_extract work/keys/prod.keys "base.nsp" work/run/mk8d/upd/update.nsp work/run/mk8d_u/romfs_dir          # the RomFS as a directory (12 GB)
DM_GAME=0100152000022000 .venv/bin/python /path/to/nxnative/dm/gfx/tools/shader_split.py \
    work/run/mk8d_u/romfs_dir/RomFS work/run/mk8d_u/romfs_dir/ExeFS/main work/run/mk8d_u/pkg pkg_map.tsv pkg_facts.tsv   # 29,651 shaders, a minute
python3 tools/shader_pkg_to_dump.py work/run/mk8d_u/pkg work/run/mk8d_u/shd_pkg
python3 tools/shader_capture_build.py work/run/mk8d_u/shd_pkg work/run/mk8d_u/mk8d.nxsl --alt-only                      # translate and pack, about 10 minutes
```

- **The program key is not stable.** The host key (SHA-256 of the control section and the code) changes from run to run: control bytes at +0x778 and from +0x7d8 differ. The library is keyed by
  the hash of the code alone, and the host looks a shader up by it when the program key misses (`prog_alt_key`), then aliases the program key (`nvn_shader_alias`).
- **Translator failures are silent in the summary.** `translated: N ok, M failed` hides the reason; run `dotnet work/dmsh_xlate/dmsh_xlate.dll jobs.json out` on the failures (`DMSH_STACK=1` for the stack).
  Nine shaders, among them the vertex shader of the track (it reads a texture), failed with an `InvalidCastException` in Ryujinx's SPIR-V back end: the optimizer replaced the temporary that
  carries a call argument by its expression. Fixed in `third_party/ryujinx` (`AstOptimizer.IsCallArgument`).
- **Storage buffers.** `cb_offset` of a reflected storage buffer is in words: `cb_offset * 4` is a byte offset in driver segment c[0], and the slot is read off it (0x310 + 0x10 slot for
  compute; 0x210 + 0x10 slot is a uniform slot the shader reads as storage, `NVN_SB_SBUF_UBO`). A shader the packer cannot resolve gets `needs` set and the host skips it: a compute dispatch
  that ignored `needs` hung the GPU (a page fault, `ring gfx_0.0.0 timeout`).
- MK8D's compute code has no header (instructions from +0) and its thread group is 32x1x1 (control +0x748), not the 64x1x1 the builder used to assume.

## Why the ground was black

The frame is a light pre-pass renderer: a shadow pass (depth only), a G-buffer pass (5 targets), light accumulation into transient targets, and a second pass over the same meshes (depth
EQUAL) that reads the light. The title makes new texture objects every frame over the same pool memory and draws into one while reading another: on the hardware it is one image. The host
made an image per object, so the shadow map and the light buffers the materials read were empty and everything that depends on them was black (signs and the stands barely do).
`mtl_tex_init` now makes an object over the storage and description of an existing one an alias of its image (`NX_NO_TEX_SHARE=1` restores one image per object); an image is not freed while an
alias names it. Found by dumping every target and every texture a draw reads at the end of one frame (`NX_DRAW_TRACE`) and comparing storage addresses.

## Where the time goes (race, Radeon 660M, about 19 ms a frame)

Measured with the host's own sampling profiler (`NX_PROF`, `tools/prof_report.py`; guest program counters too, with `--log`). What was tried, and what it did to the median frame time of the race:

| Change | Frame time |
|---|---|
| Start of the day (RTX 2050, everything by default) | 118 ms |
| Host-cached read-back buffer, reused (present was 50 ms of it) | 78 ms |
| Empty render-target storage cleared instead of de-swizzled; dead textures evicted in one ordered pass; `recheck` without hashing | 62 -> 41 ms |
| No cutting of the submission by default (`NX_CUT=0`): every submit costs the driver a walk over its memory objects | 41 -> 29.7 ms |
| Asynchronous presentation (the game waits for the frame before, not for the GPU) | 29.7 -> 20 ms |
| One condition variable per core in the thread scheduler | `core_acquire` 4.7 -> 1.7 % of the CPU |
| Image memory from shared blocks | about 1.7 ms |

No gain, and left as they were: inline exclusive accesses (`fastmem_exclusive_access`), fewer Jits in the pool (`NX_JITS` 3 and 4 against 8). The guest's own code is about 39 % of the CPU and is spread over the whole of `main` (no block above 0.7 %); the one
hot spot, 6 % of the guest, is the SDK's mutex code (`TryLock` failing in a polling loop, `sdk+0x96f80`).

## Debugging aids

All are environment variables; nothing costs anything when unset.

| Variable | Does |
|---|---|
| `NX_NVN_API=<maj.min>` | the NVN API version the device reports (53.313 for the update, 52.32 for the base) |
| `NX_DRAW_TRACE=<n>` (+ `NX_DRAW_TRACE_DETAIL=1`, `NX_FRAME_DUMP=<prefix>`) | one line per draw of the frame whose draws run with `n` presents behind (3599 is the 3600th), with its targets, state, program keys, vertex attributes, texture handles and serials; with a dump prefix, every target it drew into and every texture a small or large draw read go to `<prefix>.<n+1>.rt<serial>.ppm` (RGBA8 and RGBA16F), with format, share of non-zero bytes and storage address; `trace dispatch` / `trace copy` lines list compute and copies |
| `NX_DRAW_SKIP=<a>-<b>` | with `NX_DRAW_TRACE`, the draws of that frame numbered `a` to `b` are made ready but not drawn |
| `NX_CUT=<n>` | draws between the segments the recorded work is sent to the GPU in (default 0 = one submission per frame; 64 helps only a GPU the host leaves idle: it cost 10 ms on the Radeon) |
| `NX_PRESENT_SYNC=1`, `NX_PRESENT_ASYNC=1` | the window texture is read back synchronously / asynchronously (default: asynchronous for the window, synchronous for the memory sink and whenever `NX_FRAME_DUMP` is set) |
| `NX_MEM_DEDICATED=1` | every image gets its own device memory instead of a piece of a shared 64 MB block (about 1.7 ms slower in the race) |
| `NX_PROF=<us>` | a sampling profiler (`src/host/prof.c`): every period the running threads are interrupted and their stacks kept; `kill -USR2 <pid>` writes `NX_PROF_OUT` (default `nx_prof.txt`), `tools/prof_report.py FILE nxlinux` reads it |
| `NX_PRESENT_STATS=1` | every 300 presents: GPU read-back, BGRA to RGBA and window wait, per frame |
| `NX_NO_TEX_SHARE=1`, `NX_NO_INVARIANT=1`, `NX_DEPTH_EQUAL_LEQUAL=1`, `NX_DEAD_MB=<n>` | switches off image sharing / the Invariant decoration of vertex positions; tests depth-equal as less-or-equal; the VRAM budget of finalized textures (default 1536) |
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
| `NX_HID_TAP=<bit>[:<s>[:<ms>]]` | press that `NpadButton` bit (0 = A) for 150 ms (or `<ms>`) every `<s>` seconds (default 4), for runs nobody sits at; `0:4:3500` goes through every menu into a Grand Prix and keeps accelerating in the race |
| `NX_TEX_TRACE=<serial>` | log when that texture is initialized and finalized, and at each upload the hash of its storage and how many of its 64 KB pages hold data |
| `NX_TEX_RAW=<serial>` | with `NX_FRAME_DUMP`: at every dumped frame the GPU's own view of that texture (a blit to RGBA8) goes to `<prefix>.<n>.gpu_<serial>.ppm`, and at frame 1000 its level 0 de-swizzled as raw blocks (`<prefix>.raw_<serial>.bin`) and its storage as the title wrote it (`<prefix>.swz_<serial>.bin`); the trace lines of draws (`NX_DRAW_TRACE_DETAIL=1`) carry each texture's NVN format, size and levels |
| `NX_POOL_LOG=1`, `NX_POOL_EAGER=1` | log each memory pool when the host imports it (base, size) / import every pool when the game makes it, as before, instead of when the GPU first needs it |
| `NX_FS_LOG=1` | every read of the RomFS image: offset, length and the guest address it goes to |
| `NX_SLOWMEM=1`, `NX_JIT_NOOPT=1` | the Jit without fast memory (every access goes through the callbacks) / without dynarmic's IR and dispatch optimizations: switches to tell an emulation fault from the title's own |
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
- **Skip draws to find which one paints the wrong pixels, then dump what it samples.** `NX_DRAW_TRACE=<n>` with `NX_DRAW_SKIP=<a>-<b>` on a frame that stands still
  (the title screen, frame 900) showed that three small draws made the confetti of blocks in the main menu's art, not the big background texture the eye went to; their textures
  were ASTC. Then take the raw blocks (`NX_TEX_RAW`) and decode them with an independent decoder (a Python one in a throwaway venv will do) next to the host's: the host's ASTC decoder
  got the trit and quint values of most blocks wrong, and a fuzz of random blocks against the other decoder (`tests/test_astc.c` keeps a few) found nothing else.

## The cost of a submission grows with the device memory allocations

Every `vkAllocateMemory` is a buffer object the driver walks on each `vkQueueSubmit`: measured at about 1.7 us per allocation and per submission, so with 1 860 live allocations a submission took 3.1 ms and with 940, 1.4 ms. A frame makes two (the game's, and the copy of the window texture). The game makes some 2 300 NVN memory pools (1 600 of at most 64 KB, 850 MB in all), and the host imported each as soon as it was made; most are never used by the GPU. Importing a pool when the GPU first needs it (`pool_ensure` in `nvn_vk.c`) took the live allocations from 1 860 to 570, the submission from 4.1 to 0.8 ms and the race from 20 ms to 16.5 ms a frame (p50; p95 20 ms). The statistics line "one-shot submissions" and the lines "device memory: file:line holds N live allocations" (every 1 800 frames) show what is left: about 280 pools and 160 buffers made by `vkbuf_create`.

Not done because it is not safe: sharing one import among neighbouring pools (81 windows of 16 MB would cover all pools). The guest memory is one mapping the host `madvise`s with `MADV_DONTNEED` when the game unmaps, so a window kept across pool lifetimes could keep the old pages.

## Frame time (Mario Kart 8 Deluxe, 1080p, dGPU)

Measured with `NX_PRESENT=null NX_HID_TAP=0:4:3500 NX_SKIP_LOG=1` plus `NX_FRAME_DUMP=...` and the per-block log line added by commit 4002439:
`per frame: N render passes, M full barriers; replay X ms, submit Y ms; ...`. The game reaches a race at about frame 3000.

- Baseline (before 4002439): about 130 ms per frame in the race, ~100 ms of it in the emulator translating graphics commands to Vulkan, ~10 ms game code.
- After 4002439 (per-draw driver allocations replaced by a transient arena): about 124 ms per frame in the race. Menus went to ~50 ms but have no earlier baseline.
- The replay line showed the real cost was not the arena: 591 draws per frame, each spending ~100 us looking up a shader in a linear list of 2,839 entries (pointer chase, cache misses) and another ~25 us in the pipeline list. Hashing both (plus a thread-local last-key cache) cut the replay from ~76 ms to ~23 ms in the race.
- The remaining ~90 ms per frame was the GPU waiting on ~1000 render passes and ~150 full barriers per frame: the game switches targets before almost every draw, and every non-draw operation (including `OP_BARRIER`) ended the open pass. Making `OP_BARRIER` no longer end the pass and deferring the pass-end barrier collapsed marks into flushes; the pass count and emitted barriers both dropped (to ~2-16 in the menus and ~16-59 in the race).
- The texture table evicted render targets before their first use (they were finalized during the course load and swept before the race used them) -> black asphalt and `attach fail ... (swept from the table)`. Fixed by not evicting a dead texture until it has been touched at least once, plus a 1536 MB dead pool.
- The graphics bugs that remain identified but unfixed: 14 render targets whose NVN format has no Vulkan counterpart (`no image`), and a runtime-generated BC3 texture the game writes through an `R32G32B32A32_UINT` view of a `VK_FORMAT_BC3_UNORM_BLOCK` image (validation: incompatible format compatibility class) -- Vulkan needs two aliased images over the same memory (one BC3, one block-sized integer).
- Fixed since: the 14 render targets were the 3-component NVN formats (0x19-0x24, RGB8/RGB16/RGB32) that `nvn_fmt_vk` left without a Vulkan counterpart; they now substitute to RGBA8/RGBA16/RGBA32, and the upload path expands 3 -> 4 bytes per pixel (RGB -> RGBA, alpha = 1) in both the linear and the block-linear staging. The BC3 alias fix (commit 239ca8e) makes the draw execute and copy back into the compressed image. After both, the skip reasons observed in a dGPU race run are no longer texture-format related: the remaining ones are render targets that still fail to attach (serials such as 6000), and the frame count / replay time keep dropping.
- The machine has a discrete `GeForce RTX 2050` that the emulator never used: `VK_DRIVER_FILES` was pinned to the Radeon ICD. Selecting it is `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json` (or `NX_GPU` once both ICDs are visible). With it the race still spends ~90 ms per frame on barriers/passes, so the barrier work is the priority, not the device.

NX_GPU is already honoured by `src/gfx/vk/vkctx.c` (it picks device index `NX_GPU`); the ICD override is the part that needs the environment.
