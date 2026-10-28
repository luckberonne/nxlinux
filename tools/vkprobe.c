// Prints what this machine's Vulkan drivers offer for the NVN backend: devices, the features and extensions the design
// relies on, texture format support (BC, ASTC, depth-stencil), and memory heaps. NX_GPU=<index> picks a device.
#include <vulkan/vulkan_core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *yesno(int b) { return b ? "yes" : "NO "; }

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static void formats(VkPhysicalDevice pd) {
    struct { VkFormat f; const char *name; } tab[] = {
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1"}, {VK_FORMAT_BC2_UNORM_BLOCK, "BC2"}, {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"},
        {VK_FORMAT_BC4_UNORM_BLOCK, "BC4"}, {VK_FORMAT_BC5_UNORM_BLOCK, "BC5"}, {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H"},
        {VK_FORMAT_BC7_UNORM_BLOCK, "BC7"},
        {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC 4x4"}, {VK_FORMAT_ASTC_6x6_UNORM_BLOCK, "ASTC 6x6"}, {VK_FORMAT_ASTC_8x8_UNORM_BLOCK, "ASTC 8x8"},
        {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2"},
        {VK_FORMAT_D32_SFLOAT, "D32"}, {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8"}, {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"}, {VK_FORMAT_D16_UNORM, "D16"},
        {VK_FORMAT_X8_D24_UNORM_PACK32, "X8D24"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"}, {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "R11G11B10F"}, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10"},
        {VK_FORMAT_R5G6B5_UNORM_PACK16, "RGB565"}, {VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1R5G5B5"}, {VK_FORMAT_R4G4B4A4_UNORM_PACK16, "RGBA4"},
        {VK_FORMAT_B8G8R8A8_SRGB, "BGRA8 sRGB"}, {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"},
    };
    printf("  formats (sampled / color or depth attachment / blendable):\n");
    for (size_t i = 0; i < sizeof tab / sizeof *tab; i++) {
        VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(pd, tab[i].f, &p);
        VkFormatFeatureFlags f = p.optimalTilingFeatures;
        printf("    %-12s %s %s %s\n", tab[i].name, yesno(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
               yesno(f & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)), yesno(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT));
    }
}

int main(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkprobe", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { fprintf(stderr, "vkCreateInstance failed\n"); return 1; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pds[16]; if (n > 16) n = 16; vkEnumeratePhysicalDevices(inst, &n, pds);
    printf("%u Vulkan device(s)\n", n);
    for (uint32_t d = 0; d < n; d++) {
        VkPhysicalDevice pd = pds[d];
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
        VkExtensionProperties *ext = calloc(ne, sizeof *ext); vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);

        VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
        VkPhysicalDeviceDepthClipControlFeaturesEXT dcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &xfb};
        VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &dcc};
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        VkPhysicalDeviceFeatures *f = &f2.features;

        VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &emh};
        vkGetPhysicalDeviceProperties2(pd, &p2);

        printf("\nGPU%u: %s (%s), Vulkan %u.%u.%u\n", d, p.deviceName,
               p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : "other",
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
        printf("  core features: bufferDeviceAddress %s  descriptorIndexing(runtimeArray %s, updateAfterBind sampled %s, partiallyBound %s)\n",
               yesno(f12.bufferDeviceAddress), yesno(f12.runtimeDescriptorArray), yesno(f12.descriptorBindingSampledImageUpdateAfterBind), yesno(f12.descriptorBindingPartiallyBound));
        printf("                 timelineSemaphore %s  dynamicRendering %s  synchronization2 %s  shaderInt64 %s  shaderFloat64 %s  shaderDemoteToHelper %s\n",
               yesno(f12.timelineSemaphore), yesno(f13.dynamicRendering), yesno(f13.synchronization2), yesno(f->shaderInt64), yesno(f->shaderFloat64), yesno(f13.shaderDemoteToHelperInvocation));
        printf("                 geometryShader %s  tessellationShader %s  dualSrcBlend %s  independentBlend %s  imageCubeArray %s  samplerAnisotropy %s\n",
               yesno(f->geometryShader), yesno(f->tessellationShader), yesno(f->dualSrcBlend), yesno(f->independentBlend), yesno(f->imageCubeArray), yesno(f->samplerAnisotropy));
        printf("                 fragmentStoresAndAtomics %s  storageImageWriteWithoutFormat %s  multiDrawIndirect %s  drawIndirectFirstInstance %s  depthClamp %s  fillModeNonSolid %s\n",
               yesno(f->fragmentStoresAndAtomics), yesno(f->shaderStorageImageWriteWithoutFormat), yesno(f->multiDrawIndirect), yesno(f->drawIndirectFirstInstance), yesno(f->depthClamp), yesno(f->fillModeNonSolid));
        printf("                 textureCompressionBC %s  textureCompressionASTC_LDR %s  textureCompressionETC2 %s\n",
               yesno(f->textureCompressionBC), yesno(f->textureCompressionASTC_LDR), yesno(f->textureCompressionETC2));
        printf("  extensions: transform_feedback %s (feature %s)  depth_clip_control %s (feature %s)  depth_clip_enable %s  external_memory_host %s\n",
               yesno(has_ext(ext, ne, "VK_EXT_transform_feedback")), yesno(xfb.transformFeedback), yesno(has_ext(ext, ne, "VK_EXT_depth_clip_control")), yesno(dcc.depthClipControl),
               yesno(has_ext(ext, ne, "VK_EXT_depth_clip_enable")), yesno(has_ext(ext, ne, "VK_EXT_external_memory_host")));
        printf("              swapchain %s  memory_budget %s  custom_border_color %s  provoking_vertex %s  extended_dynamic_state3 %s  conservative_raster %s  mesh_shader %s\n",
               yesno(has_ext(ext, ne, "VK_KHR_swapchain")), yesno(has_ext(ext, ne, "VK_EXT_memory_budget")), yesno(has_ext(ext, ne, "VK_EXT_custom_border_color")),
               yesno(has_ext(ext, ne, "VK_EXT_provoking_vertex")), yesno(has_ext(ext, ne, "VK_EXT_extended_dynamic_state3")), yesno(has_ext(ext, ne, "VK_EXT_conservative_rasterization")),
               yesno(has_ext(ext, ne, "VK_EXT_mesh_shader")));
        printf("  external memory host: min import alignment %llu bytes\n", (unsigned long long)emh.minImportedHostPointerAlignment);
        printf("  limits: maxPerStageDescriptorSampledImages %u  maxDescriptorSetSampledImages %u  maxBoundDescriptorSets %u  maxStorageBufferRange %u  maxImageDimension2D %u\n",
               p.limits.maxPerStageDescriptorSampledImages, p.limits.maxDescriptorSetSampledImages, p.limits.maxBoundDescriptorSets, p.limits.maxStorageBufferRange, p.limits.maxImageDimension2D);
        formats(pd);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
            printf("  heap %u: %.2f GB%s\n", h, mp.memoryHeaps[h].size / 1073741824.0, (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            VkMemoryPropertyFlags fl = mp.memoryTypes[t].propertyFlags;
            printf("  type %u (heap %u):%s%s%s%s\n", t, mp.memoryTypes[t].heapIndex, (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " device-local" : "",
                   (fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " host-visible" : "", (fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " host-coherent" : "", (fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " host-cached" : "");
        }
        free(ext);
    }
    vkDestroyInstance(inst, NULL);
    return 0;
}
