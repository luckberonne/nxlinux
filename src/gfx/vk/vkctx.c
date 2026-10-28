#include "vkctx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(...) do { fprintf(stderr, "[RED] vk: " __VA_ARGS__); fputc('\n', stderr); return -1; } while (0)

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

int vkctx_create(vkctx_t *c) {
    memset(c, 0, sizeof *c);
    const char *layers[1]; uint32_t nl = 0;
    uint32_t lc = 0; vkEnumerateInstanceLayerProperties(&lc, NULL);
    VkLayerProperties *lp = calloc(lc ? lc : 1, sizeof *lp); vkEnumerateInstanceLayerProperties(&lc, lp);
    for (uint32_t i = 0; i < lc; i++) if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) { layers[nl++] = "VK_LAYER_KHRONOS_validation"; c->validation = 1; }
    free(lp);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxlinux", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledLayerCount = nl, .ppEnabledLayerNames = layers};
    if (vkCreateInstance(&ici, NULL, &c->inst)) FAIL("vkCreateInstance failed");

    uint32_t n = 0; vkEnumeratePhysicalDevices(c->inst, &n, NULL);
    if (!n) FAIL("no Vulkan device");
    VkPhysicalDevice pds[16]; if (n > 16) n = 16; vkEnumeratePhysicalDevices(c->inst, &n, pds);
    const char *sel = getenv("NX_GPU"); uint32_t gi = sel ? (uint32_t)atoi(sel) : 0;
    if (gi >= n) FAIL("NX_GPU=%u but only %u device(s)", gi, n);
    c->pd = pds[gi];
    vkGetPhysicalDeviceProperties(c->pd, &c->props);
    vkGetPhysicalDeviceMemoryProperties(c->pd, &c->mem);

    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(c->pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne, sizeof *ext); vkEnumerateDeviceExtensionProperties(c->pd, NULL, &ne, ext);
    const char *want[12]; uint32_t nw = 0;
    c->has_ext_mem_host = has_ext(ext, ne, "VK_EXT_external_memory_host");
    c->has_xfb = has_ext(ext, ne, "VK_EXT_transform_feedback");
    c->has_depth_clip_control = has_ext(ext, ne, "VK_EXT_depth_clip_control");
    if (c->has_ext_mem_host) want[nw++] = "VK_EXT_external_memory_host";
    if (c->has_xfb) want[nw++] = "VK_EXT_transform_feedback";
    if (c->has_depth_clip_control) want[nw++] = "VK_EXT_depth_clip_control";
    int has_interlock = has_ext(ext, ne, "VK_EXT_fragment_shader_interlock");
    if (has_interlock) want[nw++] = "VK_EXT_fragment_shader_interlock";
    c->has_push_descriptor = has_ext(ext, ne, "VK_KHR_push_descriptor");
    if (c->has_push_descriptor) want[nw++] = "VK_KHR_push_descriptor";
    free(ext);

    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, has_interlock ? &il : NULL};
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &xfb};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &dcc};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(c->pd, &f2);
    VkPhysicalDeviceFeatures *f = &f2.features;
    // What the NVN backend cannot do without
    if (!f12.bufferDeviceAddress) FAIL("%s lacks bufferDeviceAddress", c->props.deviceName);
    if (!f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingSampledImageUpdateAfterBind)
        FAIL("%s lacks the descriptor indexing features bindless textures need", c->props.deviceName);
    if (!f13.dynamicRendering || !f13.synchronization2) FAIL("%s lacks dynamic rendering or synchronization2", c->props.deviceName);
    if (!f12.timelineSemaphore) FAIL("%s lacks timeline semaphores", c->props.deviceName);
    if (!f->textureCompressionBC) FAIL("%s lacks BC texture compression", c->props.deviceName);
    c->has_xfb = c->has_xfb && xfb.transformFeedback;
    c->has_depth_clip_control = c->has_depth_clip_control && dcc.depthClipControl;

    // Enable the features we use (all checked above or optional); leave the rest off
    // The translator declares the capabilities of its whole instruction set in every module, so a device without them cannot take translated shaders at all
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT eil = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, NULL, has_interlock && il.fragmentShaderPixelInterlock,
                                                              has_interlock && il.fragmentShaderSampleInterlock, has_interlock && il.fragmentShaderShadingRateInterlock};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT exfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, has_interlock ? &eil : NULL, c->has_xfb, c->has_xfb};
    VkPhysicalDeviceDepthClipControlFeaturesEXT edcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &exfb, c->has_depth_clip_control};
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &edcc, .dynamicRendering = 1, .synchronization2 = 1,
                                            .shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation};
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13, .bufferDeviceAddress = 1, .timelineSemaphore = 1,
        .runtimeDescriptorArray = 1, .shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing,
        .descriptorBindingPartiallyBound = 1, .descriptorBindingSampledImageUpdateAfterBind = 1, .descriptorBindingUpdateUnusedWhilePending = f12.descriptorBindingUpdateUnusedWhilePending,
        .descriptorBindingVariableDescriptorCount = f12.descriptorBindingVariableDescriptorCount, .descriptorIndexing = 1, .scalarBlockLayout = f12.scalarBlockLayout};
    VkPhysicalDeviceFeatures ef = {.geometryShader = f->geometryShader, .tessellationShader = f->tessellationShader, .dualSrcBlend = f->dualSrcBlend,
        .independentBlend = f->independentBlend, .shaderInt64 = f->shaderInt64, .shaderFloat64 = f->shaderFloat64, .shaderImageGatherExtended = f->shaderImageGatherExtended,
        .shaderStorageImageReadWithoutFormat = f->shaderStorageImageReadWithoutFormat, .samplerAnisotropy = f->samplerAnisotropy,
        .fragmentStoresAndAtomics = f->fragmentStoresAndAtomics, .textureCompressionBC = 1, .depthClamp = f->depthClamp, .fillModeNonSolid = f->fillModeNonSolid,
        .multiDrawIndirect = f->multiDrawIndirect, .imageCubeArray = f->imageCubeArray, .shaderStorageImageWriteWithoutFormat = f->shaderStorageImageWriteWithoutFormat,
        .drawIndirectFirstInstance = f->drawIndirectFirstInstance, .logicOp = f->logicOp};
    VkPhysicalDeviceFeatures2 ef2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12, .features = ef};

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &nq, NULL);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &nq, qp);
    c->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if ((qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { c->qfam = i; break; }
    if (c->qfam == UINT32_MAX) FAIL("no graphics+compute queue family");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = c->qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = nw, .ppEnabledExtensionNames = want};
    if (vkCreateDevice(c->pd, &dci, NULL, &c->dev)) FAIL("vkCreateDevice failed");
    vkGetDeviceQueue(c->dev, c->qfam, 0, &c->queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->qfam};
    if (vkCreateCommandPool(c->dev, &pci, NULL, &c->pool)) FAIL("vkCreateCommandPool failed");

    if (c->has_ext_mem_host) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &emh};
        vkGetPhysicalDeviceProperties2(c->pd, &p2);
        c->host_import_align = emh.minImportedHostPointerAlignment;
        c->GetMemoryHostPointerProperties = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(c->dev, "vkGetMemoryHostPointerPropertiesEXT");
    }
    c->enabled = ef;
    if (c->has_push_descriptor) c->CmdPushDescriptorSet = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->dev, "vkCmdPushDescriptorSetKHR");
    if (c->has_xfb) {
        c->CmdBeginTransformFeedback = (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(c->dev, "vkCmdBeginTransformFeedbackEXT");
        c->CmdEndTransformFeedback = (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(c->dev, "vkCmdEndTransformFeedbackEXT");
        c->CmdBindTransformFeedbackBuffers = (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(c->dev, "vkCmdBindTransformFeedbackBuffersEXT");
    }
    return 0;
}

void vkctx_destroy(vkctx_t *c) {
    if (c->dev) { vkDeviceWaitIdle(c->dev); vkDestroyCommandPool(c->dev, c->pool, NULL); vkDestroyDevice(c->dev, NULL); }
    if (c->inst) vkDestroyInstance(c->inst, NULL);
    memset(c, 0, sizeof *c);
}

uint32_t vkctx_memtype(const vkctx_t *c, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c->mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (c->mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int vkbuf_create(vkctx_t *c, vkbuf_t *b, VkDeviceSize size, VkBufferUsageFlags usage, int host) {
    memset(b, 0, sizeof *b); b->size = size;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(c->dev, &bi, NULL, &b->buf)) FAIL("vkCreateBuffer failed");
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c->dev, b->buf, &mr);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t mt = vkctx_memtype(c, mr.memoryTypeBits, want);
    if (mt == UINT32_MAX) FAIL("no memory type for a %s buffer", host ? "host" : "device");
    VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size, mt};
    if (vkAllocateMemory(c->dev, &ai, NULL, &b->mem)) FAIL("vkAllocateMemory failed");
    vkBindBufferMemory(c->dev, b->buf, b->mem, 0);
    if (host && vkMapMemory(c->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map)) FAIL("vkMapMemory failed");
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo di = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b->buf};
        b->addr = vkGetBufferDeviceAddress(c->dev, &di);
    }
    return 0;
}
void vkbuf_destroy(vkctx_t *c, vkbuf_t *b) {
    if (b->map) vkUnmapMemory(c->dev, b->mem);
    vkDestroyBuffer(c->dev, b->buf, NULL); vkFreeMemory(c->dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

int vkimg_create2d(vkctx_t *c, vkimg_t *i, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, uint32_t levels) {
    memset(i, 0, sizeof *i); i->fmt = fmt; i->w = w; i->h = h;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(c->dev, &ii, NULL, &i->img)) FAIL("vkCreateImage failed");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(c->dev, i->img, &mr);
    uint32_t mt = vkctx_memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) FAIL("no device-local memory type for an image");
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    if (vkAllocateMemory(c->dev, &ai, NULL, &i->mem)) FAIL("vkAllocateMemory failed");
    vkBindImageMemory(c->dev, i->img, i->mem, 0);
    int depth = fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    if (vkCreateImageView(c->dev, &vi, NULL, &i->view)) FAIL("vkCreateImageView failed");
    return 0;
}
void vkimg_destroy(vkctx_t *c, vkimg_t *i) {
    vkDestroyImageView(c->dev, i->view, NULL); vkDestroyImage(c->dev, i->img, NULL); vkFreeMemory(c->dev, i->mem, NULL);
    memset(i, 0, sizeof *i);
}

VkCommandBuffer vkctx_begin(vkctx_t *c) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(c->dev, &ai, &cb)) return NULL;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
int vkctx_end(vkctx_t *c, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
    vkCreateFence(c->dev, &fi, NULL, &fence);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VkResult r = vkQueueSubmit(c->queue, 1, &si, fence);
    if (!r) r = vkWaitForFences(c->dev, 1, &fence, VK_TRUE, 10ull * 1000000000ull);
    vkDestroyFence(c->dev, fence, NULL);
    vkFreeCommandBuffers(c->dev, c->pool, 1, &cb);
    if (r) FAIL("submit or wait failed (%d)", (int)r);
    return 0;
}

VkShaderModule vkctx_shader_from_file(vkctx_t *c, const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return VK_NULL_HANDLE; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)n);
    if (fread(code, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(code); return VK_NULL_HANDLE; }
    fclose(f);
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->dev, &si, NULL, &m)) fprintf(stderr, "[RED] vk: vkCreateShaderModule failed for %s\n", path);
    free(code);
    return m;
}

void vk_barrier_image(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da, VkImageAspectFlags aspect, uint32_t levels) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa, .dstStageMask = ds, .dstAccessMask = da,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {aspect, 0, levels, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

void vk_barrier_image_all(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa, .dstStageMask = ds, .dstAccessMask = da,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}
