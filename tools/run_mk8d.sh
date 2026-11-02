#!/bin/sh
# The shader library work/run/mk8d_u/mk8d.nxsl holds every shader of the game files: see docs/mk8d-bringup.md ("The shader library").
# Starts Mario Kart 8 Deluxe from the extraction tools/run_title.py left in work/run/mk8d_u (base + 4.x update, a 64-bit process).
# Build first: cmake -S . -B build-NAME -G Ninja && cmake --build build-NAME. Usage: tools/run_mk8d.sh [BUILD_DIR]   (a window by default; NX_PRESENT=null runs without one)
B=${1:-build-claude}
cd "$(dirname "$0")/.." || exit 1
SH=""; [ -f work/run/mk8d_u/mk8d.nxsl ] && SH="--shaders work/run/mk8d_u/mk8d.nxsl"       # the translated shader library: tools/shader_capture_build.py over an NX_SHADER_DUMP run
AOC=""; [ -f work/run/mk8d_dlc/aoc.romfs ] && AOC="--aoc 0100152000023001:work/run/mk8d_dlc/aoc.romfs"   # the Booster Course Pass: nx_extract KEYS --aoc DLC.nsp work/run/mk8d_dlc/aoc.romfs --romfs-image
# The Vulkan validation layers cost about half the frame rate and are on whenever they are installed: off here, VK_LOADER_LAYERS_DISABLE= (empty) turns them back on
export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE-VK_LAYER_KHRONOS_validation}
# On a laptop with an AMD iGPU and an NVIDIA dGPU (the RTX 2050 here) the iGPU is the faster one for this: the race takes 16 ms a frame on the Radeon 660M against 25-29 ms on the
# power-limited RTX (docs/next-steps.md, 2026-10-31), and with only the AMD driver loaded the NVIDIA stays asleep, which leaves the shared power budget to the CPU the emulator is bound by.
# So unless VK_DRIVER_FILES or NX_GPU says otherwise, only the AMD Vulkan driver is loaded when there is one; VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json runs on the NVIDIA.
if [ -z "${VK_DRIVER_FILES+x}" ] && [ -z "$NX_GPU" ] && [ -f /usr/share/vulkan/icd.d/radeon_icd.json ]; then export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json; fi
NX_PRESENT=${NX_PRESENT:-sdl} NX_NVN_API=${NX_NVN_API:-53.313} exec "$B/nxlinux" --manifest work/run/mk8d_u/modules.txt --romfs work/run/mk8d_u/extract/RomFS.img $SH $AOC
