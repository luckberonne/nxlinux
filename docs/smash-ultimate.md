# Super Smash Bros. Ultimate (branch `smash-ultimate`)

Base game 1.0.0 (`01006A800016E000`, an NSZ with block compression), a 64-bit process, SDK 8.2.1. nxnative's notes on this title (`dm/games/01006A800016E000/README.md` in its checkout) were the guide: its profile and
patches are what each step below reproduces.

## Run it

```
.venv/bin/python -I tools/nsz_to_nsp.py "<the .nsz>" work/run/ssbu/base.nsp          # block NCZ: the zstd stream is bounded to its own file now
.venv/bin/python -I tools/run_title.py --keys work/keys/prod.keys --nsp work/run/ssbu/base.nsp --work work/run/ssbu --romfs-image --no-run
build-claude/nxlinux --manifest work/run/ssbu/modules.txt --romfs work/run/ssbu/extract/RomFS.img --sys-resource 0x1000000 [--shaders work/run/ssbu/ssbu.nxsl]
```

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

## State

The title now runs its frame loop at about 57 frames/s headless with no abort, loads its NROs (`ldr:ro: NRO mapped at ...`), and presents, but **draws nothing**: every frame is compute work (`OP_BIND_UBO_SMALL`, `OP_BIND_SSBO`, `OP_BIND_IMG`) with 0 render passes and the window texture stays at alpha 0.

- **Next blocker (deterministic, not the JIT mode, not the scheduler, not the NVN API version):** a worker of the job pool (threads 0x1007..0x1009, priority 51, one per core) dies about 4.8 s in with a null dereference at `main+0x7d778`. That function calls `main+0x81df0(obj, id, size, slot)`, which looks a slot up in a table of the game's (`obj+0x1888+slot` is an "enabled" byte, memset to 0 at creation and never set before the failing use) and gets NULL. What sets the byte is not known; the object is `0x20c23b4068` with index `0x25a1`. Ways in: `NX_WATCH` on the byte (the host crashed under the watch in one run), a trace of the game's writes before the call, or comparing with what nxnative does around the same code (it has the title running).
- **Then:** `OP_BIND_IMG` (storage images for compute) is recorded but not executed in the replay (262 144 calls and counting), `uploading a depth-stencil texture from storage` (NVN format 0x35), and the hid command 109 and Mii database command 22 stubs.
- The base game is 1.0.0; nxnative's notes are for 13.0.5. Its `data.arc` reader knows only 13.x's file system table, which is irrelevant here (the host serves the RomFS as one image) but means the NRO collection of nxnative has no counterpart: the host lets `nn::ro` load the NROs the game reads itself.
- Regression checks run after the memory-layout changes: MK8D boots and renders (frames 100..3000), the 21 tests pass.
