// The shaders the Vulkan backend can run, by the 8-byte key nvn programs carry (the first 8 bytes of SHA-256 of a shader's control section and code,
// hle_nvn.c keys_hash). The offline chain (dmsh_xlate: Maxwell -> SPIR-V) will fill this from its output; until it does, tests register their own.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { NVN_STAGE_VERTEX = 0, NVN_STAGE_FRAGMENT = 1, NVN_STAGE_GEOMETRY = 2, NVN_STAGE_TESS_CONTROL = 3, NVN_STAGE_TESS_EVAL = 4, NVN_STAGE_COMPUTE = 5 };   // NVNshaderStage
enum { NVN_SB_CBUF = 1, NVN_SB_TEX = 2 };    // binding kinds (dmsh_abi.h DMSH_KIND_*); more as the draw path grows: storage buffers, the driver segments

// Vulkan binding in descriptor set 0, what it is, and its slot (a cbuf's uniform slot; a combined texture's c2 slot). aux: for a texture, the translator's SamplerType (3 = 2D)
typedef struct { uint32_t binding; uint32_t kind; uint32_t reg; uint32_t aux; } nvn_shbind;

// What a shader reads that the draw path cannot supply yet (shader_pack.py): a draw with such a shader is skipped and says which
enum { NVN_NEED_C0 = 1, NVN_NEED_C1 = 2, NVN_NEED_C2 = 4, NVN_NEED_SBUF = 8, NVN_NEED_TEX = 16, NVN_NEED_IMG = 32, NVN_NEED_GEOM = 64 };

typedef struct {
    int stage;                    // NVN_STAGE_*
    uint32_t needs;               // NVN_NEED_* bits
    const uint32_t *spirv; size_t bytes;
    const nvn_shbind *binds; int nbinds;
} nvn_shader_desc;

void nvn_vk_shader_register(const uint8_t key[8], const nvn_shader_desc *d);   // copies everything; registering a key again replaces it
uint64_t nvn_vk_shader_key(const uint8_t key[8]);                               // 8 bytes as a number, for tables

// A shader library built by tools/shader_pack.py from the translator's output. Returns how many shaders were registered, or -1 if the file cannot be used.
int nvn_vk_shader_load_library(const char *path);
