# Vulkan backend notes

Probe of this machine (`tools/vkprobe.c`, output in `docs/vkprobe-radv.txt`): AMD Radeon 660M iGPU on RADV, Vulkan 1.4.

## What the design relies on, and it is there
bufferDeviceAddress, descriptor indexing (runtime arrays, update-after-bind, partially bound), timeline semaphores, dynamic
rendering, synchronization2, transform feedback, depth_clip_control, geometry and tessellation shaders, dual-source blending,
independent blending, multi-draw indirect, `VK_EXT_external_memory_host`.

`VK_EXT_external_memory_host` has a minimum import alignment of 4096 bytes, the guest page size. NVN memory pools (CPU-visible
memory with a GPU address) can therefore be imported straight from the guest window with no copy.

## What it lacks, and the plan
- ASTC and ETC2 are not supported (desktop AMD). Games that ship ASTC need a compute decoder to RGBA8 (or a BC7 transcode).
  Mario Kart 8 Deluxe uses BC, which is supported for sampling (not as attachments, as expected).
- `D24_UNORM_S8_UINT` and `X8_D24` are not supported: map to `D32_SFLOAT_S8_UINT` / `D32_SFLOAT`.
- Memory: 2.6 GB host-visible GTT and a 5.3 GB device-local carve-out, all types host-visible-coherent where it matters.

## Open
- The RTX 2050 does not enumerate through Vulkan in this session: `nvidia-smi` sees it and the NVIDIA ICD is installed, but
  the loader lists only RADV, also with `__NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only`. Development targets RADV.
- The Vulkan validation layers are not installed (`vulkan-validation-layers`), so tests run without them.
