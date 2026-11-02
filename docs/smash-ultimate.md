# Super Smash Bros. Ultimate (branch `smash-ultimate`)

Base game 1.0.0 (`01006A800016E000`, an NSZ with block compression), a 64-bit process, SDK 8.2.1. nxnative's notes on this title (`dm/games/01006A800016E000/README.md` in its checkout) were the guide: its profile and
patches are what each step below reproduces.

## Run it

```
.venv/bin/python -I tools/nsz_to_nsp.py "<the .nsz>" work/run/ssbu/base.nsp          # block NCZ: the zstd stream is bounded to its own file now
.venv/bin/python -I tools/run_title.py --keys work/keys/prod.keys --nsp work/run/ssbu/base.nsp --work work/run/ssbu --romfs-image --no-run
build-claude/nxlinux --manifest work/run/ssbu/modules.txt --romfs work/run/ssbu/extract/RomFS.img --sys-resource 0x1000000 [--shaders work/run/ssbu/ssbu.nxsl]
```

`tools/run_smash.sh [BUILD_DIR]` does the last line with the shader library, the Radeon and a shared font that has U+25A1 (see the table).

Shaders: `NX_SHADER_DUMP=work/run/ssbu/shd` on a run, then `python3 -I tools/shader_capture_build.py work/run/ssbu/shd work/run/ssbu/ssbu.nxsl` (6187 shaders translated without failures, 22 s).
`--sys-resource` is the NPDM's SystemResourceSize, which `run_title.py` now passes by itself (16 MB here).

## What the host needed, in the order the title asked for it

| Symptom | Cause | Change |
|---|---|---|
| abort in the static initialisers | the engine reserves 62 GB with `nn::os::AllocateAddressRegion` | alias region of 64 GB, only reserved (`main.c`, `NX_ALIAS_MB`); `alias_query` skips whole bitmap words |
| SDK did not build its virtual address memory | NPDM SystemResourceSize was not given to `GetInfo` | `--sys-resource`, read from `main.npdm` by `run_title.py` |
| abort inside the SDK's log observer | `LogImpl` -> lm observer walks the module with QueryMemory and an assertion fails | `hle_diag.c`: the game's log lines go to the host log (`[game] ...`) |
| `friend:u` abort | the Uid overload of `GetFriendList` | `hle_friends.c` |
| `audout:u` and `ldr:ro` missing | services | `ipc_audout.c` (cmds 7 and 8, the Auto forms, are Smash's), `ldr_ro` in `ipc_services.c` (RegisterModuleInfo, MapManualLoadModuleMemory identity-mapped with the Jit's cache dropped, Unmap) |
| `CreateThread` aborted | a thread stack is the `.bss` of `main`; `MapMemory` accepts a module's data/bss only if `data_off` is known, and `main.c` passed the NSO segment offsets only to the 32-bit loader | `main.c` passes them for 64-bit modules too (this also makes QueryMemory report code, read-only and data apart) |
| abort in game code, `GetTouchScreenState` | the SDK read the empty hid ring and left `count` undefined | `hle_hid.c`: no touches |
| null call in `fs::EnsureSaveData` | SDK 8.2.1's path uses a service object the host does not set up | `hle_fs_save.c`: succeeds (the save directory is made when it is opened) |
| `Break 0x80000003` killed the process | PreLoadLibrary notifications (bit 31) must return | `mem.c` `default_break` |
| main thread spun at 130 M SVC/s | it yields with `SleepThread(-1)` waiting for a lower-priority worker of its core | `kernel.c`: yields -1/-2 leave the core for 100 us so any waiter runs |
| `kernel: handle table full` | `friends::NotificationQueue::GetSystemEvent` and `GetFriendInvitationNotificationEvent` bound their static event to a new kernel pair on every call (about 17 a second) | `hle_friends.c` binds once; the table now says which kind and which creator piled up |
| a worker of the job pool died with a null dereference at `main+0x7d778` (4.8 s in) | the game builds a glyph cache over the shared fonts (`nn::fontll`) and looks up U+25A1 (the square) in each; the host's shared font was Noto Sans, which has no such glyph, so the lookup returned NULL and the caller used it | `NX_SHARED_FONT` with a font that has it: `tools/run_smash.sh` picks one with `fc-match 'sans:charset=25a1'` (DejaVu Sans). Not the default: Mario Kart 8 Deluxe's font code faults on DejaVu |
| host SIGSEGV at a guest address | `nvnCommandBufferSetMemoryCallback`'s function is the game's AArch64 code and `cmd_alloc_keep` called it as a native function | `hle_nvn.c`: `cpu_call64` when the pointer is in the guest window |
| null call in `fs::GetSaveDataSize` | the same missing service object as EnsureSaveData | `hle_fs_save.c` answers 96 MB / 10 MB |
| the main thread starved for good (core 0 held 30 s by a thread that was blocked) | `SetThreadCoreMask` changed `t->core` while the thread owned another core, and `core_release` released `g_owner[t->core]`, leaving the old one owned forever | `kernel.c`: `thr_t.held` is the core owned; release and "already the owner" use it |

## State

The title runs its frame loop at about 60 frames/s headless for minutes with no abort or fault: 2 render passes and 18 draws a frame (UI quads), 388 live textures, the movie hook installed. **The picture is still black**: the colour target keeps only its alpha. Seen in a draw trace of frame 1000 (`NX_DRAW_TRACE=1000 NX_DRAW_TRACE_DETAIL=1`): 17 of the 18 draws run with a scissor of (960, 540, 0, 0) (the game records it that way, `NX_SCISSOR_LOG=1` shows the records), so nothing lands; the 18th has the full scissor and draws nothing visible either. Not known yet whether this screen is meant to be black (a fade before the opening movie, which needs the movie decoder) or something the host feeds wrong.

- **The opening movie (VP9 video + a second decoder of type 5, audio)**: the audio decoder had no output, so the player never got going (3 packets sent, then nothing). `hle_movie.c` now gives every decoder that is not a video codec silent PCM of the packet's length (Opus table-of-contents guess), and the player runs further: video packets up to pts 5.475 s (a 72 KB keyframe), where it stops again for good (the log with `NX_MOVIE_LOG=1 NX_MOVIE_LOG_ALL=1` ends with three input buffers acquired and never sent; the threads 0x5c/0x5d poll a condition variable 50,000 times a second). The video texture the first draw of every frame samples (1280x720 R8 + 640x360 RG8, linear, storage in a pool) stays all zero, although the decoded frames handed to the game have content (Y 188). Not found yet: what the player waits for after that keyframe (the extractor's file reads? an audio clock?), and who copies the frame into the texture.
- **Next (in this order):** what the game waits for at the black screen (the threads at 20 s are mostly parked on condition variables; the main thread keeps running); whether the opening movie plays (`hle_movie.c` has the decoder replacement; its logging is off by default); `OP_BIND_IMG` (storage images for compute) is recorded but not executed in the replay (262 144 calls and counting); `uploading a depth-stencil texture from storage` (NVN format 0x35) and `a stencil clear with a partial mask`; the hid command 109 and Mii database command 22 stubs.
- The base game is 1.0.0; nxnative's notes are for 13.0.5. Its `data.arc` reader knows only 13.x's file system table, which is irrelevant here (the host serves the RomFS as one image) but means the NRO collection of nxnative has no counterpart: the host lets `nn::ro` load the NROs the game reads itself.
- Regression checks run after the memory-layout changes: MK8D boots and renders (frames 100..3000), the 21 tests pass.
