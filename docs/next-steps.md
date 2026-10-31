# Next steps

Where Mario Kart 8 Deluxe stands on nxlinux and what to do next. The debugging switches and methods are in [mk8d-bringup.md](mk8d-bringup.md); the controls are in the README.

## State

- The title, menus and a Grand Prix race in Mario Kart Stadium run and look right on the Radeon 660M (RADV) and the RTX 2050 (the RTX is power-limited and about twice as slow).
- Race: about 20 ms a frame (50 fps) with `NX_PRESENT_ASYNC=1`, no dumps. About 17 ms of that is the frame's work: the guest (JIT) and the replay. In the replay, 638 draws, 55 render passes and 129 full barriers a frame cost 2.1 ms of replay and 3.9 ms of submission.
- Controls: keyboard (WASD, K/J/I/U, Q/E, 1/3, arrows, P/O) and a gamepad with a driving layout (README).

## Speed

To reach 60 fps the race needs about 3.3 ms less a frame. Most of the frame is the guest's own code, which the JIT knobs did not shorten (inline exclusives, a smaller Jit pool: no gain).

Measured and ruled out:

- **A persistent `VkPipelineCache`** (tried on 2026-10-31): the file loads and is 1.7 MB, but the start-up stalls did not move (the longest frame stayed at about 450 ms, p95 the same). Those frames build no pipelines, upload no textures and spend 14 ms in submission: the time is in the guest (file reads and decompression), not in the graphics. RADV also keeps its own shader cache. Not worth the code.
- **A frame dump slows the race to 31-35 ms** (the presenter reads the window back synchronously whenever `NX_FRAME_DUMP` is set). Measure speed without dumps.

Still worth measuring before touching code (`NX_PROF`, the recipe in mk8d-bringup.md):

1. **Submission, 3.9 ms**: fewer render passes (55) and barriers (129) a frame.
2. **Texture churn**: 5 019 live, 30 711 dead and 150 742 swept textures at frame 5100; what `mtl_tex_init` and the sweeps cost a frame.
3. **The long frames in the guest** (frame 395, 2920, 3232: 450 ms with a quiet host): which guest code runs then (`NX_PROF` guest sampling at those frames) and whether the RomFS reads (`NX_FS_LOG`, 512 KB each) or the Yaz0 decompression are the cost.
4. The SDK mutex `TryLock` polling (about 6 % of the guest, 0.7 ms at most): last, it is risky for the gain.

Not worth doing yet: ASTC decoding on the GPU (no cost showed in the frame breakdown).

## Graphics and games

- Check more screens at full size, not in thumbnails: results, pause, Time Trials, battle, the other cups. The method that found the ASTC fault: `NX_DRAW_TRACE=<n> NX_DRAW_SKIP=a-b` on a still frame, then `NX_TEX_RAW=<serial>` against an independent decoder.
- Extend the ASTC fuzz (4x4 only so far) to the other block sizes and keep the script in `tools/`.
- Cup icons: reported as not visible by the player; not reproduced in dumps or in the window's renderer (`NX_SDL_READBACK`). Needs the exact screen and a window capture.
- `OP_RENDER_COND` is recorded and never executed (always draws); occlusion-tested effects only.
- Validation errors left: render area larger than the attachment (`VkRenderingInfo-pNext-06079/06080`), mip views out of range (`VkImageViewCreateInfo-subresourceRange-01478/01718`), an image in UNDEFINED layout when sampled (`vkCmdDraw-09600`). Integer vertex attributes declared as float (`Input-08733`) are harmless.
- Log warnings: depth-stencil upload from storage (format 0x35), `RegisterTexture` with a NULL texture, `hid` commands 202/206, Mii database command 21.

## Everyday use

- Window: fullscreen and screenshot keys, a `--gpu` option instead of `VK_DRIVER_FILES`, frame pacing with a 21 ms game under a 60 Hz window.
- Audio and saves: audio has been off in every test; check timing at 50 fps and that progress persists (`NX_SAVE_DIR`).
- Long runs (30 minutes in a race and in the menus) for leaks and hangs.

## Method

- Build only in `build-claude`; commit exact paths; one-line commit messages; no game files, keys or firmware in the repository.
- Measure a speed change with two rounds per variant and compare the `timing up to frame 4500/4800/5100` lines; a single run is +-1.5 ms.
- Check the window's own pixels (alpha included: the dumps drop the alpha channel), not only the dumps.
