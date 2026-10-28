// NVNformat (nvn.h 0x01-0x9C) -> Vulkan. The table is written entry by entry in NVN enum order; unlisted formats (including 0 and out of range) return 0.
// Component remapping: "no alpha" formats such as RGBX / RGB_DXT1 read a as 1; RGB-family 16-bit packed formats swap r/b; all others are identity.
//
// 16-bit packed formats: Metal names them from the lowest bit up, Vulkan from the highest, so nxnative's ABGR4 is R4G4B4A4_PACK16 here,
// BGR5A1 is A1R5G5B5, B5G6R5 is R5G6B5 and A1BGR5 is R5G5B5A1. The bit order against real data is as unverified as it is there.
#include "nvn_fmt_vk.h"

typedef struct { uint16_t vk; uint8_t kind, noalpha, rb; } ent;   // vk: low bits of the VkFormat enum (see fmt()); kind: 1 exact, 2 substitute
#define E(m) {m, 1, 0, 0}
#define X(m) {m, 1, 1, 0}     // exact, no alpha
#define S(m) {m, 2, 0, 0}     // substitute
#define SX(m) {m, 2, 1, 0}
#define N {0, 0, 0, 0}        // no counterpart
#define SR(m) {m, 2, 0, 1}    // substitute, swap r/b
#define SXR(m) {m, 2, 1, 1}

// The table holds indices into this list so that it stays one line per format; formats outside the core enum range are listed explicitly
enum {
    f_none, R8_UNORM, R8_SNORM, R8_UINT, R8_SINT, R16_UNORM, R16_SNORM, R16_UINT, R16_SINT, R16_SFLOAT,
    R32_UINT, R32_SINT, R32_SFLOAT, RG8_UNORM, RG8_SNORM, RG8_UINT, RG8_SINT, RG16_UNORM, RG16_SNORM, RG16_UINT, RG16_SINT, RG16_SFLOAT,
    RG32_UINT, RG32_SINT, RG32_SFLOAT, RGBA8_UNORM, RGBA8_SNORM, RGBA8_UINT, RGBA8_SINT, RGBA8_SRGB, BGRA8_UNORM, BGRA8_SRGB,
    RGBA16_UNORM, RGBA16_SNORM, RGBA16_UINT, RGBA16_SINT, RGBA16_SFLOAT, RGBA32_UINT, RGBA32_SINT, RGBA32_SFLOAT,
    R4G4B4A4, R5G6B5, R5G5B5A1, A1R5G5B5, A2B10G10R10_UNORM, A2B10G10R10_UINT, B10G11R11_UFLOAT, E5B9G9R9_UFLOAT,
    BC1_RGBA, BC1_RGBA_SRGB, BC2, BC2_SRGB, BC3, BC3_SRGB, BC4_U, BC4_S, BC5_U, BC5_S, BC6H_U, BC6H_S, BC7, BC7_SRGB,
    S8, D16, D32, D32S8,
};
static const VkFormat F[] = {
    [f_none] = VK_FORMAT_UNDEFINED,
    [R8_UNORM] = VK_FORMAT_R8_UNORM, [R8_SNORM] = VK_FORMAT_R8_SNORM, [R8_UINT] = VK_FORMAT_R8_UINT, [R8_SINT] = VK_FORMAT_R8_SINT,
    [R16_UNORM] = VK_FORMAT_R16_UNORM, [R16_SNORM] = VK_FORMAT_R16_SNORM, [R16_UINT] = VK_FORMAT_R16_UINT, [R16_SINT] = VK_FORMAT_R16_SINT, [R16_SFLOAT] = VK_FORMAT_R16_SFLOAT,
    [R32_UINT] = VK_FORMAT_R32_UINT, [R32_SINT] = VK_FORMAT_R32_SINT, [R32_SFLOAT] = VK_FORMAT_R32_SFLOAT,
    [RG8_UNORM] = VK_FORMAT_R8G8_UNORM, [RG8_SNORM] = VK_FORMAT_R8G8_SNORM, [RG8_UINT] = VK_FORMAT_R8G8_UINT, [RG8_SINT] = VK_FORMAT_R8G8_SINT,
    [RG16_UNORM] = VK_FORMAT_R16G16_UNORM, [RG16_SNORM] = VK_FORMAT_R16G16_SNORM, [RG16_UINT] = VK_FORMAT_R16G16_UINT, [RG16_SINT] = VK_FORMAT_R16G16_SINT, [RG16_SFLOAT] = VK_FORMAT_R16G16_SFLOAT,
    [RG32_UINT] = VK_FORMAT_R32G32_UINT, [RG32_SINT] = VK_FORMAT_R32G32_SINT, [RG32_SFLOAT] = VK_FORMAT_R32G32_SFLOAT,
    [RGBA8_UNORM] = VK_FORMAT_R8G8B8A8_UNORM, [RGBA8_SNORM] = VK_FORMAT_R8G8B8A8_SNORM, [RGBA8_UINT] = VK_FORMAT_R8G8B8A8_UINT, [RGBA8_SINT] = VK_FORMAT_R8G8B8A8_SINT,
    [RGBA8_SRGB] = VK_FORMAT_R8G8B8A8_SRGB, [BGRA8_UNORM] = VK_FORMAT_B8G8R8A8_UNORM, [BGRA8_SRGB] = VK_FORMAT_B8G8R8A8_SRGB,
    [RGBA16_UNORM] = VK_FORMAT_R16G16B16A16_UNORM, [RGBA16_SNORM] = VK_FORMAT_R16G16B16A16_SNORM, [RGBA16_UINT] = VK_FORMAT_R16G16B16A16_UINT,
    [RGBA16_SINT] = VK_FORMAT_R16G16B16A16_SINT, [RGBA16_SFLOAT] = VK_FORMAT_R16G16B16A16_SFLOAT,
    [RGBA32_UINT] = VK_FORMAT_R32G32B32A32_UINT, [RGBA32_SINT] = VK_FORMAT_R32G32B32A32_SINT, [RGBA32_SFLOAT] = VK_FORMAT_R32G32B32A32_SFLOAT,
    [R4G4B4A4] = VK_FORMAT_R4G4B4A4_UNORM_PACK16, [R5G6B5] = VK_FORMAT_R5G6B5_UNORM_PACK16, [R5G5B5A1] = VK_FORMAT_R5G5B5A1_UNORM_PACK16, [A1R5G5B5] = VK_FORMAT_A1R5G5B5_UNORM_PACK16,
    [A2B10G10R10_UNORM] = VK_FORMAT_A2B10G10R10_UNORM_PACK32, [A2B10G10R10_UINT] = VK_FORMAT_A2B10G10R10_UINT_PACK32,
    [B10G11R11_UFLOAT] = VK_FORMAT_B10G11R11_UFLOAT_PACK32, [E5B9G9R9_UFLOAT] = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
    [BC1_RGBA] = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, [BC1_RGBA_SRGB] = VK_FORMAT_BC1_RGBA_SRGB_BLOCK, [BC2] = VK_FORMAT_BC2_UNORM_BLOCK, [BC2_SRGB] = VK_FORMAT_BC2_SRGB_BLOCK,
    [BC3] = VK_FORMAT_BC3_UNORM_BLOCK, [BC3_SRGB] = VK_FORMAT_BC3_SRGB_BLOCK, [BC4_U] = VK_FORMAT_BC4_UNORM_BLOCK, [BC4_S] = VK_FORMAT_BC4_SNORM_BLOCK,
    [BC5_U] = VK_FORMAT_BC5_UNORM_BLOCK, [BC5_S] = VK_FORMAT_BC5_SNORM_BLOCK, [BC6H_U] = VK_FORMAT_BC6H_UFLOAT_BLOCK, [BC6H_S] = VK_FORMAT_BC6H_SFLOAT_BLOCK,
    [BC7] = VK_FORMAT_BC7_UNORM_BLOCK, [BC7_SRGB] = VK_FORMAT_BC7_SRGB_BLOCK,
    [S8] = VK_FORMAT_S8_UINT, [D16] = VK_FORMAT_D16_UNORM, [D32] = VK_FORMAT_D32_SFLOAT, [D32S8] = VK_FORMAT_D32_SFLOAT_S8_UINT,
};

static const ent T[0x9D] = {
    [0x01] = E(R8_UNORM), E(R8_SNORM), E(R8_UINT), E(R8_SINT),
    [0x05] = E(R16_SFLOAT), E(R16_UNORM), E(R16_SNORM), E(R16_UINT), E(R16_SINT),
    [0x0A] = E(R32_SFLOAT), E(R32_UINT), E(R32_SINT),
    [0x0D] = E(RG8_UNORM), E(RG8_SNORM), E(RG8_UINT), E(RG8_SINT),
    [0x11] = E(RG16_SFLOAT), E(RG16_UNORM), E(RG16_SNORM), E(RG16_UINT), E(RG16_SINT),
    [0x16] = E(RG32_SFLOAT), E(RG32_UINT), E(RG32_SINT),
    // 0x19-0x24 RGB8 / RGB16 / RGB32 three-component: sampling support for 3-component images is not universal, so they return 0 as in nxnative.
    // Handling: on upload expand per pixel to the 4-component format of the same kind (RGBA8 / RGBA16 / RGBA32, a = 1; for integer kinds a = raw value 1), sampled as RGBX;
    //           when used as a buffer texture (target BUFFER), the shader reads the 3 components manually instead.
    [0x19] = N, N, N, N, N, N, N, N, N, N, N, N,
    [0x25] = E(RGBA8_UNORM), E(RGBA8_SNORM), E(RGBA8_UINT), E(RGBA8_SINT),
    [0x29] = E(RGBA16_SFLOAT), E(RGBA16_UNORM), E(RGBA16_SNORM), E(RGBA16_UINT), E(RGBA16_SINT),
    [0x2E] = E(RGBA32_SFLOAT), E(RGBA32_UINT), E(RGBA32_SINT),
    [0x31] = E(S8), E(D16),
    // DEPTH24: desktop AMD has no D24 (VK_FORMAT_D24_UNORM_S8_UINT and X8_D24 are unsupported) -> D32_SFLOAT: as a render target this is a precision upgrade with
    // equivalent behavior; marked substitute only because uploading existing 24-bit depth data requires conversion to float (render targets are not affected)
    [0x33] = S(D32), E(D32),
    // DEPTH24_STENCIL8 -> D32_SFLOAT_S8_UINT (as above); DEPTH32F_STENCIL8 is exact
    [0x35] = S(D32S8), E(D32S8),
    [0x37] = X(RGBA8_SRGB), E(RGBA8_SRGB),
    // 16-bit packed formats, see the note at the top of this file. All marked substitute: verify the bit order when a real texture is first encountered
    [0x39] = S(R4G4B4A4), SXR(A1R5G5B5), SR(A1R5G5B5), SR(R5G6B5),
    [0x3D] = E(A2B10G10R10_UNORM), E(A2B10G10R10_UINT), E(B10G11R11_UFLOAT), E(E5B9G9R9_UFLOAT),
    // DXT / RGTC / BPTC = BC1-7; RGB_DXT1 has no alpha
    [0x41] = X(BC1_RGBA), E(BC1_RGBA), E(BC2), E(BC3), X(BC1_RGBA_SRGB), E(BC1_RGBA_SRGB), E(BC2_SRGB), E(BC3_SRGB),
    [0x49] = E(BC4_U), E(BC4_S), E(BC5_U), E(BC5_S), E(BC7), E(BC7_SRGB), E(BC6H_S), E(BC6H_U),
    // *_UI2F / *_I2F: integer storage, read as float (conversion without normalization). Vulkan has no such read mode -> substituted with the same-width integer
    // format; the shader samples as integer and then converts to float. 3-component ones are handled like the 0x19 group (return 0)
    [0x51] = S(R8_UINT), S(R8_SINT), S(R16_UINT), S(R16_SINT), S(R32_UINT), S(R32_SINT),
    [0x57] = S(RG8_UINT), S(RG8_SINT), S(RG16_UINT), S(RG16_SINT), S(RG32_UINT), S(RG32_SINT),
    [0x5D] = N, N, N, N, N, N,
    [0x63] = S(RGBA8_UINT), S(RGBA8_SINT), S(RGBA16_UINT), S(RGBA16_SINT), S(RGBA32_UINT), S(RGBA32_SINT),
    // RGB10A2 signed / signed integer: sampling support for A2B10G10R10_SNORM / SINT is optional, as nxnative has no Metal counterpart -> 0;
    // handling: unpack to RGBA16Snorm / RGBA16Sint on upload. RGB10A2_UI2F -> A2B10G10R10_UINT with the shader converting to float (substitute); RGB10A2_I2F -> 0
    [0x69] = N, N, S(A2B10G10R10_UINT), N,
    [0x6D] = X(RGBA8_UNORM), X(RGBA8_SNORM), X(RGBA8_UINT), X(RGBA8_SINT),
    [0x71] = X(RGBA16_SFLOAT), X(RGBA16_UNORM), X(RGBA16_SNORM), X(RGBA16_UINT), X(RGBA16_SINT),
    [0x76] = X(RGBA32_SFLOAT), X(RGBA32_UINT), X(RGBA32_SINT),
    // 0x79-0x94 ASTC: computed below (two groups, LDR / sRGB, 14 each)
    [0x95] = S(R5G6B5), SX(A1R5G5B5), S(A1R5G5B5), S(R5G5B5A1),
    [0x99] = X(BGRA8_UNORM), E(BGRA8_UNORM), X(BGRA8_SRGB), E(BGRA8_SRGB),
};

// NVN ASTC order: 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12 -- the same order as Vulkan's, which is not true of Metal (it skips a number)
static const VkFormat astc_ldr[14] = {
    VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
    VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_UNORM_BLOCK,
    VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
static const VkFormat astc_srgb[14] = {
    VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VK_FORMAT_ASTC_5x4_SRGB_BLOCK, VK_FORMAT_ASTC_5x5_SRGB_BLOCK, VK_FORMAT_ASTC_6x5_SRGB_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
    VK_FORMAT_ASTC_8x5_SRGB_BLOCK, VK_FORMAT_ASTC_8x6_SRGB_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK, VK_FORMAT_ASTC_10x5_SRGB_BLOCK, VK_FORMAT_ASTC_10x6_SRGB_BLOCK,
    VK_FORMAT_ASTC_10x8_SRGB_BLOCK, VK_FORMAT_ASTC_10x10_SRGB_BLOCK, VK_FORMAT_ASTC_12x10_SRGB_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK};

int nvn_fmt_to_vk(int f, VkFormat *vk, VkComponentSwizzle swz[4]) {
    ent e = {0, 0, 0, 0};
    VkFormat out = VK_FORMAT_UNDEFINED;
    if (f >= 0x79 && f <= 0x94) { int k = (f - 0x79) % 14; out = f < 0x87 ? astc_ldr[k] : astc_srgb[k]; e = (ent){1, 1, 0, 0}; }
    else if (f > 0 && f < (int)(sizeof T / sizeof *T)) { e = T[f]; if (e.kind) out = F[e.vk]; }
    if (vk) *vk = out;
    if (swz) {
        swz[0] = e.rb ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_R; swz[1] = VK_COMPONENT_SWIZZLE_G;
        swz[2] = e.rb ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_B; swz[3] = e.noalpha ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_A;
    }
    return e.kind;
}

int nvn_fmt_vk_supported(VkPhysicalDevice pd, VkFormat f) {
    if (f == VK_FORMAT_UNDEFINED) return 0;
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    VkFormatFeatureFlags want = (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT)
        ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return (p.optimalTilingFeatures & want) == want;
}
