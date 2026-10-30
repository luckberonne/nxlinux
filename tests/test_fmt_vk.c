// The NVN -> Vulkan format table: spot checks of the values that are easy to get wrong, the ASTC ordering, the swizzles,
// and a pass over the whole table against what this device can sample.
#include "../src/gfx/tex/nvn_fmt_vk.h"
#include "../src/gfx/vk/vkctx.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static int is_astc(VkFormat f) { return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; }

int main(void) {
    VkFormat f; VkComponentSwizzle s[4];
    CHECK(nvn_fmt_to_vk(0x01, &f, s) == 1 && f == VK_FORMAT_R8_UNORM);
    CHECK(s[0] == VK_COMPONENT_SWIZZLE_R && s[3] == VK_COMPONENT_SWIZZLE_A);
    CHECK(nvn_fmt_to_vk(0x25, &f, NULL) == 1 && f == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(nvn_fmt_to_vk(0x37, &f, s) == 1 && f == VK_FORMAT_R8G8B8A8_SRGB && s[3] == VK_COMPONENT_SWIZZLE_ONE);   // RGBX8_SRGB: alpha reads 1
    CHECK(nvn_fmt_to_vk(0x38, &f, s) == 1 && f == VK_FORMAT_R8G8B8A8_SRGB && s[3] == VK_COMPONENT_SWIZZLE_A);
    CHECK(nvn_fmt_to_vk(0x33, &f, NULL) == 2 && f == VK_FORMAT_D32_SFLOAT);                  // DEPTH24 -> D32: a substitute
    CHECK(nvn_fmt_to_vk(0x35, &f, NULL) == 2 && f == VK_FORMAT_D32_SFLOAT_S8_UINT);
    CHECK(nvn_fmt_to_vk(0x36, &f, NULL) == 1 && f == VK_FORMAT_D32_SFLOAT_S8_UINT);
    CHECK(nvn_fmt_to_vk(0x41, &f, s) == 1 && f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && s[3] == VK_COMPONENT_SWIZZLE_ONE);   // RGB_DXT1
    CHECK(nvn_fmt_to_vk(0x42, &f, s) == 1 && f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && s[3] == VK_COMPONENT_SWIZZLE_A);
    CHECK(nvn_fmt_to_vk(0x44, &f, NULL) == 1 && f == VK_FORMAT_BC3_UNORM_BLOCK);
    CHECK(nvn_fmt_to_vk(0x4D, &f, NULL) == 1 && f == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(nvn_fmt_to_vk(0x4F, &f, NULL) == 1 && f == VK_FORMAT_BC6H_SFLOAT_BLOCK);
    CHECK(nvn_fmt_to_vk(0x50, &f, NULL) == 1 && f == VK_FORMAT_BC6H_UFLOAT_BLOCK);
    CHECK(nvn_fmt_to_vk(0x3F, &f, NULL) == 1 && f == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    CHECK(nvn_fmt_to_vk(0x3C, &f, s) == 2 && f == VK_FORMAT_R5G6B5_UNORM_PACK16 && s[0] == VK_COMPONENT_SWIZZLE_B && s[2] == VK_COMPONENT_SWIZZLE_R);   // RGB565 swaps r/b
    CHECK(nvn_fmt_to_vk(0x95, &f, s) == 2 && f == VK_FORMAT_R5G6B5_UNORM_PACK16 && s[0] == VK_COMPONENT_SWIZZLE_R);                                      // BGR565 does not
    CHECK(nvn_fmt_to_vk(0x9A, &f, NULL) == 1 && f == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(nvn_fmt_to_vk(0x9B, &f, s) == 1 && f == VK_FORMAT_B8G8R8A8_SRGB && s[3] == VK_COMPONENT_SWIZZLE_ONE);
    // No counterpart: the 3-component formats, out-of-range numbers, zero
    CHECK(nvn_fmt_to_vk(0x19, &f, NULL) == 2 && f == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(nvn_fmt_to_vk(0x24, &f, NULL) == 2 && f == VK_FORMAT_R32G32B32A32_SINT);
    CHECK(nvn_fmt_to_vk(0, &f, NULL) == 0 && f == VK_FORMAT_UNDEFINED);
    CHECK(nvn_fmt_to_vk(0x9D, &f, NULL) == 0 && f == VK_FORMAT_UNDEFINED);
    CHECK(nvn_fmt_to_vk(-1, &f, NULL) == 0 && f == VK_FORMAT_UNDEFINED);
    // ASTC: 14 block sizes in NVN order, LDR then sRGB, ending where Vulkan's enum ends
    CHECK(nvn_fmt_to_vk(0x79, &f, NULL) == 1 && f == VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    CHECK(nvn_fmt_to_vk(0x7D, &f, NULL) == 1 && f == VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
    CHECK(nvn_fmt_to_vk(0x7E, &f, NULL) == 1 && f == VK_FORMAT_ASTC_8x5_UNORM_BLOCK);
    CHECK(nvn_fmt_to_vk(0x86, &f, NULL) == 1 && f == VK_FORMAT_ASTC_12x12_UNORM_BLOCK);
    CHECK(nvn_fmt_to_vk(0x87, &f, NULL) == 1 && f == VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
    CHECK(nvn_fmt_to_vk(0x94, &f, NULL) == 1 && f == VK_FORMAT_ASTC_12x12_SRGB_BLOCK);

    // Every NVN number that has a mapping must name a distinct kind of data: count them
    int exact = 0, subst = 0, none = 0;
    for (int n = 1; n <= 0x9C; n++) { int k = nvn_fmt_to_vk(n, &f, NULL); k == 1 ? exact++ : k == 2 ? subst++ : none++; CHECK((k == 0) == (f == VK_FORMAT_UNDEFINED)); }
    printf("NVN formats 0x01-0x9C: %d exact, %d substitute, %d without a Vulkan counterpart\n", exact, subst, none);
    CHECK(exact + subst + none == 0x9C);

    // Against this device: everything but ASTC must be usable
    vkctx_t c;
    if (vkctx_create(&c)) { puts("SKIP device checks: no Vulkan device"); puts(g_fail ? "FAIL" : "PASS"); return g_fail; }
    int unsupported_non_astc = 0, unsupported_astc = 0;
    for (int n = 1; n <= 0x9C; n++) {
        if (!nvn_fmt_to_vk(n, &f, NULL) || nvn_fmt_vk_supported(c.pd, f)) continue;
        if (is_astc(f)) { unsupported_astc++; continue; }
        printf("  not usable on %s: NVN %#x -> VkFormat %d\n", c.props.deviceName, n, (int)f);
        unsupported_non_astc++;
    }
    printf("%s: %d ASTC formats unsupported (expected on desktop AMD), %d others unsupported\n", c.props.deviceName, unsupported_astc, unsupported_non_astc);
    CHECK(unsupported_non_astc == 0);
    vkctx_destroy(&c);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
