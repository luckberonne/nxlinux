// Headless Vulkan context for the NVN backend: instance, device with the features the design relies on, one graphics+compute
// queue, a command pool, and small helpers for memory, buffers, images and one-shot command buffers.
#pragma once
#include <vulkan/vulkan_core.h>
#include <stdint.h>

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkCommandPool pool;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    int validation;                 // the Khronos validation layer is active
    int has_xfb, has_depth_clip_control, has_ext_mem_host;
    VkDeviceSize host_import_align; // minImportedHostPointerAlignment
    PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerProperties;
    PFN_vkCmdBeginTransformFeedbackEXT CmdBeginTransformFeedback;
    PFN_vkCmdEndTransformFeedbackEXT CmdEndTransformFeedback;
    PFN_vkCmdBindTransformFeedbackBuffersEXT CmdBindTransformFeedbackBuffers;
} vkctx_t;

// NX_GPU=<index> picks a physical device (default 0). Returns 0 on success; prints what is missing on failure.
int vkctx_create(vkctx_t *c);
void vkctx_destroy(vkctx_t *c);

uint32_t vkctx_memtype(const vkctx_t *c, uint32_t type_bits, VkMemoryPropertyFlags want);   // UINT32_MAX if none

typedef struct { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; void *map; VkDeviceAddress addr; } vkbuf_t;
// A buffer with its own memory. host: host-visible+coherent and mapped; otherwise device-local. addr is filled when BDA usage is set.
int vkbuf_create(vkctx_t *c, vkbuf_t *b, VkDeviceSize size, VkBufferUsageFlags usage, int host);
void vkbuf_destroy(vkctx_t *c, vkbuf_t *b);

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkFormat fmt; uint32_t w, h; } vkimg_t;
int vkimg_create2d(vkctx_t *c, vkimg_t *i, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, uint32_t levels);
void vkimg_destroy(vkctx_t *c, vkimg_t *i);

// Records into a fresh primary command buffer, submits, and waits. begin returns it; end submits and frees it.
VkCommandBuffer vkctx_begin(vkctx_t *c);
int vkctx_end(vkctx_t *c, VkCommandBuffer cb);

VkShaderModule vkctx_shader_from_file(vkctx_t *c, const char *path);
void vk_barrier_image(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                      VkImageAspectFlags aspect, uint32_t levels);
