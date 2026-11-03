# Mario Party Superstars (branch `smash-ultimate`, work in progress)

Base game 1.0.0 (an NSP of 2.9 GB, a 64-bit process, SDK 12). Nothing of it is in the repository.

## Run it

```
.venv/bin/python -I tools/run_title.py --keys work/keys/prod.keys --nsp "<the .nsp>" --work work/run/mps --romfs-image --no-run
NX_SHADER_DUMP=work/run/mps/shd build-claude/nxlinux --manifest work/run/mps/modules.txt --romfs work/run/mps/extract/RomFS.img    # a couple of minutes, then close it
python3 -I tools/shader_capture_build.py work/run/mps/shd work/run/mps/mps.nxsl                                                      # 1079 shaders, 5 s
tools/run_mps.sh [BUILD_DIR]                                                                                                           # a window (NX_PRESENT=null: none)
```

`nxlinux-mps.desktop` (in `~/.local/share/applications`) starts it from KDE with the game's icon (`NX_APP_ID`, `NX_WINDOW_TITLE` name the window).

## What the host needed, in the order the title asked

| Symptom | Cause | Change |
|---|---|---|
| Break 0x1003 at the start | the SDK asks `SetHeapSize(total - used)` and `UsedMemorySize` did not count the modules, which `SetHeapSize` charges | `mem.c`: used memory includes `module_image_bytes()`; `NX_APP_POOL_MB=n` sizes the application pool if a title needs more than 3,285 MB |
| null call in `nn::applet::SetCpuBoostMode` | `oe::Initialize` is taken over (a no-op) and the SDK's applet object was never set up | `hle_oe.c` takes `SetCpuBoostMode` over |
| abort in `RequestUpdateAudioRenderer` (result 2153-0041) | the title uses audio renderer revision 8, whose response has a render info block (revision 7 and later) the host's did not carry | `audren_mix.c`: 0x10 bytes more and the size in the header's word 10 from revision 7 |
| `GetService "acc:u0"` | SDK 12's `nn::account::Initialize()` is a 4-byte tail branch (too short to take over) to `detail::InitializeForApplication` | `hle_account.c` takes the callee over |
| `GetService "irs"` then a null call | the Joy-Con infrared camera | `ipc_irs.c`: answers success, shared memory of zeros |
| the title's abort printed nothing | `nn::diag::detail::AbortImpl` ends in a Break without text | `hle_diag.c` prints expression, function, file and line when the title gives them (release builds give none) and, with `NX_BREAK_STACK=1`, the frames |

## State

It reaches its main menu (Offline Play / Local Play / Online Play) in about 10 s and draws it correctly; with A pressed it goes on to the player select ("OK!") and the story's opening cutscene, which is **real-time 3D and mostly not drawn**: the scenes show a grey screen with the subtitle, the sky and silhouettes of hills, a glowing orb. The shader library built from a 130 s dump (3035 shaders) is not the limit. Most shaders the translator flagged as needing textures (1951 of 3035) did not read their texture handle from the driver table but from one of the game's own uniform buffers (bindless: `cbuf_slot` 3, 8, 9, 11); they are now a `NVN_SB_TEX_UBO` binding (`shader_pack.py ubo_texture`, `bind_handle` in `nvn_draw_vk.c` reads the handle from the bound buffer when the draw runs), and the cutscene draws its trees, flowers, water, sky and particles. Still missing: the **ground is flat grey** (146 shaders sample arrays of separate textures through `cbuf_slot` 0 / handle type 4, `NVN_NEED_TEX`; 158 need storage images, `OP_BIND_IMG`, not executed in the replay; one draw samples more than 16 textures; depth-stencil textures cannot be uploaded from storage). Those are the next work for this title (and for Smash's 3D scenes). Not tried: the board games, input beyond A, sound (the audio renderer's revision 8 sections beyond the header are parsed as revision 4's), Local Play and Online Play (no network services).

## The 146 texture-array shaders (not done)

The last 146 shaders the translator flags (`NVN_NEED_TEX`) sample a 32-entry array of separate textures and a 32-entry array of samplers, indexed by a texture ID the game keeps in a storage buffer (Ryujinx's fallback: `fp_t2d_cb0_40000000`, element = min(ID, 31)). The first attempt filled element k with the texture and sampler of pool ID k, which removed the flag and drew the cutscene's ground. It crashed: this GPU allows **32 push descriptors per set** (`maxPushDescriptors = 32`, checked with `vulkaninfo`), and the 160 descriptors of those arrays overflow the push set. Mesa does not check it and the heap is corrupted (`malloc(): invalid size` in `prepare`). The change needs the array bindings in a regular descriptor set whose lifetime follows the submission. The attempt is kept as `work/run/mps/texarray_push_overflow.patch` (git-ignored).

