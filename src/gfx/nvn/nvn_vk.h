// Vulkan backend of the NVN layer: the entry points of nvn_mtl.h that touch the GPU. Replaces nvn_null.c function by function.
// Falls back to the null behaviour when no Vulkan device can be opened, so the layer still runs (and says so once).
#pragma once
#include <stdint.h>

int nvn_gpu_ok(void);                 // a Vulkan device is open and the backend is active
const char *nvn_gpu_name(void);       // its name, or "none"
unsigned nvn_gpu_pools(void);         // memory pools imported into Vulkan
uint64_t nvn_gpu_completed(void);     // timeline value the GPU has reached
