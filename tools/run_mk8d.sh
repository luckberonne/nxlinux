#!/bin/sh
# Starts Mario Kart 8 Deluxe from the extraction tools/run_title.py left in work/run/mk8d_u (base + 4.x update, a 64-bit process).
# Build first: cmake -S . -B build-NAME -G Ninja && cmake --build build-NAME. Usage: tools/run_mk8d.sh [BUILD_DIR]   (NX_PRESENT=null runs without a window)
B=${1:-build-claude}
cd "$(dirname "$0")/.." || exit 1
SH=""; [ -f work/run/mk8d_u/mk8d.nxsl ] && SH="--shaders work/run/mk8d_u/mk8d.nxsl"       # the translated shader library: tools/shader_capture_build.py over an NX_SHADER_DUMP run
NX_NVN_API=${NX_NVN_API:-53.313} exec "$B/nxlinux" --manifest work/run/mk8d_u/modules.txt --romfs work/run/mk8d_u/extract/RomFS.img $SH
