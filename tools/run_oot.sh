#!/bin/sh
# Starts the user's Ocarina of Time (a Switch 2 .nup, see docs/ounce-shaders.md) from the extraction in work/run/zelda_oot_probe.
# Usage: tools/run_oot.sh [BUILD_DIR]   (a window by default; NX_PRESENT=null runs without one; NX_HID_TAP=0:6 presses A now and then to skip the movies)
# The shader library is work/run/ounce_all8.nxsl (tools/ounce_pack_dir.py + tools/shader_pack.py; ounce_all8 is all7 retranslated with the TEX/TLD operand fix and the 851 programs merged in).
# NX_JIT_CACHE_MB=48 (dynarmic's default is 128): the JIT's patch tables grow with the code cache, 1.5 GB per run at the default; with 48 the world load stays under the 15 GB of the machine.
# NX_POOL_PLACED=1: the memory pools of 1 MB or more are device memory mapped at their own address (nvn_vk.c pool_place): each submission costs a fraction of what
# imported pages cost on amdgpu (title 10.6 -> 16.8 fps, world 3.3 -> 9.5 fps on the Radeon 660M). NX_POOL_PLACED=0 turns it off.
# It needs about 10 GB of free RAM: with less the kernel kills the process during the world load.
B=${1:-build-claude}
cd "$(dirname "$0")/.." || exit 1
export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE-VK_LAYER_KHRONOS_validation}
SHLIB=${SHLIB:-work/run/ounce_all8.nxsl}
NX_POOL_PLACED=${NX_POOL_PLACED-1} NX_JIT_CACHE_MB=${NX_JIT_CACHE_MB:-48} NX_PRESENT=${NX_PRESENT:-sdl} NX_HEAP_MB=${NX_HEAP_MB:-12000} exec "$B/nxlinux" --manifest work/run/zelda_oot_probe/modules.txt --romfs work/run/zelda_oot_probe/extract/RomFS.img --sys-resource 0x3000000 --shaders "$SHLIB"
