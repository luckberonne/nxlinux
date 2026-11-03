#!/bin/sh
# Starts Mario Party Superstars (base 1.0.0, a 64-bit process) from the extraction tools/run_title.py left in work/run/mps (docs/mario-party-superstars.md has the steps).
# Usage: tools/run_mps.sh [BUILD_DIR]   (a window by default; NX_PRESENT=null runs without one)
B=${1:-build-claude}
cd "$(dirname "$0")/.." || exit 1
SH=""; [ -f work/run/mps/mps.nxsl ] && SH="--shaders work/run/mps/mps.nxsl"      # tools/shader_capture_build.py over an NX_SHADER_DUMP run
export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE-VK_LAYER_KHRONOS_validation}
if [ -z "${VK_DRIVER_FILES+x}" ] && [ -z "$NX_GPU" ] && [ -f /usr/share/vulkan/icd.d/radeon_icd.json ]; then
    export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json
    [ -z "${__EGL_VENDOR_LIBRARY_FILENAMES+x}" ] && [ -f /usr/share/glvnd/egl_vendor.d/50_mesa.json ] && export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
    [ -z "${__GLX_VENDOR_LIBRARY_NAME+x}" ] && export __GLX_VENDOR_LIBRARY_NAME=mesa
fi
export NX_APP_ID=${NX_APP_ID:-nxlinux-mps} NX_WINDOW_TITLE=${NX_WINDOW_TITLE:-"Mario Party Superstars"}
NX_PRESENT=${NX_PRESENT:-sdl} exec "$B/nxlinux" --manifest work/run/mps/modules.txt --romfs work/run/mps/extract/RomFS.img $SH
