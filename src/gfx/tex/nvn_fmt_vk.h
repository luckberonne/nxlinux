// NVNformat -> VkFormat + component remapping. Port of nxnative's nvn_fmt_mtl.c: same table, same kinds, same remarks.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>

// Returns: 1 = exact match (data uploaded as is);
//          2 = substitute (precision / bit order / read mode differs: upload needs conversion or the shader must read differently);
//          0 = no Vulkan counterpart (*vk is set to VK_FORMAT_UNDEFINED): the 3-component formats, expanded to 4 components on upload.
// swz gives, for each of the four view components r/g/b/a, which source component to take (VkComponentMapping order); may be NULL.
// Support is a property of the device, not of the table: ASTC, ETC2 and 24-bit depth do not exist on desktop AMD. See nvn_fmt_vk_supported.
int nvn_fmt_to_vk(int nvn_format, VkFormat *vk, VkComponentSwizzle swz[4]);

// Whether the physical device can sample (and, for depth, attach) the format.
int nvn_fmt_vk_supported(VkPhysicalDevice pd, VkFormat f);
