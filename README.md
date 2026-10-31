# nxlinux

A Linux x86_64 port of [nxnative](https://github.com/seedds/nxnative), which runs Nintendo Switch games on Apple silicon.
nxlinux runs the game's AArch64 code in a JIT (dynarmic), answers its system calls with a Horizon high-level emulation (HLE) host,
and implements the graphics API (NVN) on Vulkan.

- **First target:** Mario Kart 8 Deluxe 4.0.1 (title `0100152000022000`).
- **Status:** work in progress. The game boots, shows its title splash and reaches the menus, and the race runs.
  Audio, the movie panels and the movie decoder are not done yet.
  See [docs/mk8d-bringup.md](docs/mk8d-bringup.md) for the detailed state and the list of what is missing.

## What is in the box

| Part | Where | Notes |
|---|---|---|
| AArch64 CPU (dynarmic) and memory | `src/host/cpu.cpp`, `third_party/dynarmic/` | Runs the guest code; no code patching of the game |
| Horizon host (loader, SVCs, threads, services) | `src/host/` | Ported from nxnative; service list in [docs/hle-services.md](docs/hle-services.md) |
| NVN on Vulkan | `src/gfx/nvn/`, `src/gfx/vk/` | Graphics API layer and Vulkan context |
| Maxwell shader translation | `tools/dmsh_xlate/`, `tools/maxwell_asm.py` | Game shaders are translated offline to SPIR-V |
| Extraction and run scripts | `tools/run_title.py`, `tools/run_mk8d.sh` | Unpack a title and start it |
| Tests | `tests/` | C/C++ tests run by CTest; Python tests run standalone |

The design and the reasoning behind it are in [docs/plan.md](docs/plan.md). The port notes are in
[docs/nvn-port-analysis.md](docs/nvn-port-analysis.md) and [docs/vulkan-notes.md](docs/vulkan-notes.md).

## Requirements

Linux x86_64 with a working Vulkan driver.

- **Required:** CMake, Ninja, a C/C++ compiler, `clang` with `lld` (builds the guest test programs for AArch64),
  `glslc` (builds the test shaders), Vulkan headers and loader, Python 3.
- **Optional:** SDL3 (the window and the audio output; without it, `NX_PRESENT=sdl` falls back to no window),
  FFmpeg with `libavcodec`, `libavutil` and `libswscale` (without it, movies play as black frames), `dotnet` (builds the
  Maxwell translator and the NSP extractor, and enables the shader test that uses them).

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
(cd build && ctest)
```

This builds `nxlinux`, the shader tools and the tests. CMake reports which optional parts were found.

## Run a title

Nothing from a console is in this repository. You bring your own keys and dumps:

- `prod.keys`, dumped from your own console (for example with Lockpick_RCM);
- the game and its update, dumped from your own cartridge or console (for example with nxdumptool).
  An update that comes as `.nsz` is converted first with `tools/nsz_to_nsp.py`.

```sh
python3 tools/run_title.py --keys PATH/prod.keys --nsp "PATH/Mario Kart 8 Deluxe.nsp" [--update PATH/update.nsp] --shaders build/shaders.nxsl
```

`run_title.py` extracts the NSP with the vendored `work/nx_extract` tool, unpacks the executable modules into a memory image,
and starts `nxlinux`. `NX_PRESENT=memory` runs without a window. `--entry SYMBOL` starts at a symbol instead of the start
of the first module, which the synthetic test titles use.

For Mario Kart 8 Deluxe, the exact steps, the shader library, the add-on content and the debugging switches are in
[docs/running.md](docs/running.md) and [docs/mk8d-bringup.md](docs/mk8d-bringup.md). Once the extraction and the shader library
exist, `tools/run_mk8d.sh` starts the game.

Keys, dumps, firmware and extracted game files are never committed. `work/`, `*.keys`, `*.nsp` and `*.nsz` are git-ignored.

## Tests

```sh
(cd build && ctest)                     # C/C++ tests
python3 -I tests/test_maxwell_xlate.py  # Python tests run one by one
```

The guest test programs in `tests/guest/` are AArch64 code that exercises the CPU, the services and NVN without any game.
`tools/synth_nsp.py` makes disposable stand-in NSP files for tests.

## Project layout

- `src/host/`: CPU driver, loader, kernel SVCs, memory, HLE services.
- `src/gfx/nvn/`: NVN layer, ported from nxnative, and its Vulkan backend.
- `src/gfx/vk/`: Vulkan context.
- `tests/`: C/C++ and Python tests; `tests/guest/` for guest programs; `tests/shaders/` for test shaders.
- `tools/`: extraction, shader translation, profiling and run scripts.
- `games/`: per-title profiles (game-specific imports and patches).
- `docs/`: plan, port notes, bring-up state, and the coordination notes for agents in [docs/agents/README.md](docs/agents/README.md).

## Contributing

The checkout is shared by several people and tools. Read [docs/agents/README.md](docs/agents/README.md) before you change anything.
Build in your own directory (`build-NAME`) rather than `build/`, and commit only the paths you changed.

## License

MIT. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Code copied from nxnative keeps its MIT license. Third-party components
(Ryujinx, LibHac, libvpx, stb_vorbis, dynarmic) keep their own licenses, listed in NOTICE.
