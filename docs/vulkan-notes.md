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

## Verified on RADV (tests/test_vk_triangle.c, tests/test_vk_hostmem.c)
- Vertices fetched through a buffer device address (`GL_EXT_buffer_reference`), dynamic rendering into RGBA8, readback:
  a solid triangle and an interpolated one render as expected.
- Zero-copy NVN memory pools: a page-aligned range of the guest window imported with `VK_EXT_external_memory_host`
  (`vkGetMemoryHostPointerPropertiesEXT` + `VkImportMemoryHostPointerInfoEXT`, memory type host-visible/coherent/cached),
  bound to a buffer with `SHADER_DEVICE_ADDRESS`. The CPU's writes are visible to the GPU, a GPU fill and a compute
  shader writing through the raw GPU address are visible through the same CPU pointer. This is the NVN model: CPU-visible
  memory with a GPU address, with no staging copy.
- `src/gfx/vk/vkctx.c` requires the features the backend cannot do without and names the missing one on failure.

## Format table and bindless textures
- `src/gfx/tex/nvn_fmt_vk.c` is nxnative's `nvn_fmt_mtl.c` retargeted: the same entries, kinds (exact / substitute / none) and
  remarks. 106 NVN formats map exactly, 29 are substitutes, 21 have no Vulkan counterpart, the same 21 that have none on Metal
  (3-component formats and the signed RGB10A2 ones). The ASTC order matches Vulkan's directly (Metal skips a number). The 16-bit
  packed formats needed their names reversed (Metal names from the lowest bit, Vulkan from the highest). `tests/test_fmt_vk.c`
  checks the table against the device: on RADV every mapped format but the 28 ASTC ones can be sampled or attached.
- `tests/test_vk_bindless.c`: a 1024-entry sampled-image array and a 16-entry sampler array, partially bound and update-after-bind,
  indexed with `nonuniformEXT` from an NVN-style handle (low 20 bits texture, the rest sampler) read through a buffer address.
  A descriptor written after the command buffer was recorded and the set bound is seen by the GPU.
