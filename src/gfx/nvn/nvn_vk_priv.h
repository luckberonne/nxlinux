// What the command replay (nvn_replay.c) needs from the Vulkan backend's submission machinery (nvn_vk.c).
#pragma once
#include "../vk/vkctx.h"
#include "nvn_obj.h"

vkctx_t *nvn_vk_ctx(void);                 // NULL when the backend runs on the null GPU
VkCommandBuffer nvn_vk_cmd(void);          // the submission being recorded; NULL outside mtl_sub_begin / mtl_sub_commit or on the null GPU
uint64_t nvn_vk_next_fence(void);          // the fence value the submission being recorded will signal
// The pool buffer that holds [addr, addr+len) and the offset of addr in it; 0 if the range is not inside one memory pool
int nvn_vk_pool_buffer(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);
// A buffer the submission reads while it runs (staging data): destroyed once the GPU has completed the submission
void nvn_vk_keep(const vkbuf_t *b);
void nvn_vk_barrier_all(VkCommandBuffer cb);   // everything before is visible to everything after
// Host-side effects of GPU completion, performed after the submission has completed on the GPU and in submission order
void nvn_vk_post_event(volatile uint32_t *val, uint32_t mode, uint32_t v);   // mode 0 writes v, 1 adds it
void nvn_vk_post_counter(uint64_t addr, uint32_t type);                       // writes the 16-byte report {counter, timestamp}
// An image the game released: destroyed once every submission that may use it has completed
void nvn_vk_defer_image(VkImage img, VkImageView view, VkDeviceMemory mem);
// A command buffer submitted and waited for at once (CPU readbacks). NULL on the null GPU.
VkCommandBuffer nvn_vk_oneshot_begin(void);
int nvn_vk_oneshot_end(VkCommandBuffer cb);
// Buffer <-> texture copies of the submission being recorded (nvn_tex_vk.c); row and img are the copy strides in bytes, 0 = tightly packed
void nvn_vk_copy_b2t(const p_copy_b2t *c, uint64_t row, uint64_t img);
void nvn_vk_copy_t2b(const p_copy_b2t *c, uint64_t row, uint64_t img);   // the record is the same; its `src` is the destination buffer
