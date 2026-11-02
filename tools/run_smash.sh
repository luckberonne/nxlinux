#!/bin/sh
# Starts Super Smash Bros. Ultimate from the extraction tools/run_title.py left in work/run/ssbu (docs/smash-ultimate.md has the steps that make it).
# Usage: tools/run_smash.sh [BUILD_DIR]   (a window by default; NX_PRESENT=null runs without one)
B=${1:-build-claude}
cd "$(dirname "$0")/.." || exit 1
SH=""; [ -f work/run/ssbu/ssbu.nxsl ] && SH="--shaders work/run/ssbu/ssbu.nxsl"            # tools/shader_capture_build.py over an NX_SHADER_DUMP run
export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE-VK_LAYER_KHRONOS_validation}
# The AMD iGPU first and Mesa's EGL/GLX, as in run_mk8d.sh (the reasons are there)
if [ -z "${VK_DRIVER_FILES+x}" ] && [ -z "$NX_GPU" ] && [ -f /usr/share/vulkan/icd.d/radeon_icd.json ]; then
    export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json
    [ -z "${__EGL_VENDOR_LIBRARY_FILENAMES+x}" ] && [ -f /usr/share/glvnd/egl_vendor.d/50_mesa.json ] && export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
    [ -z "${__GLX_VENDOR_LIBRARY_NAME+x}" ] && export __GLX_VENDOR_LIBRARY_NAME=mesa
fi
# The shared fonts: the game looks up U+25A1 in every one of them and crashes if none has it, so a font with that glyph (DejaVu Sans, not Noto Sans)
[ -z "${NX_SHARED_FONT+x}" ] && NX_SHARED_FONT=$(fc-match -f '%{file}' 'sans:charset=25a1' 2>/dev/null) && export NX_SHARED_FONT
NX_PRESENT=${NX_PRESENT:-sdl} exec "$B/nxlinux" --manifest work/run/ssbu/modules.txt --romfs work/run/ssbu/extract/RomFS.img --sys-resource 0x1000000 $SH
