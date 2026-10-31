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
// Short-lived uniform data for the submission being recorded (driver segments, support buffers): a piece of a large host-visible buffer, reused once the
// GPU has finished the submissions that read it. Returns 0 and fills *out and *map, or -1.
int nvn_vk_transient(size_t size, VkDescriptorBufferInfo *out, void **map);
// A texture readback that does not wait (nvn_tex_vk.c): begin submits the copy, wait blocks until the GPU has done it and gives the pixels (w * h * 4 bytes, tightly packed), done lets the buffer go
typedef struct { int slot; VkCommandBuffer cb; VkFence fence; uint32_t w, h; } mtl_rb_t;
int mtl_read_async_begin(const tex_t *t, const NVNcopyRegion *r, mtl_rb_t *job);   // 0, or -1 when the texture is not one the GPU owns whole (read it with mtl_read_texels)
const uint8_t *mtl_read_async_wait(const mtl_rb_t *job);
void mtl_read_async_done(mtl_rb_t *job);
// Device memory for images in pieces of shared blocks (nvn_vk.c); the pieces of an image are given back with it
struct vkmem_sub;
struct vkmem_sub *nvn_vk_mem_alloc(const VkMemoryRequirements *mr, uint32_t memtype);       // NULL when there is none
void nvn_vk_mem_free(struct vkmem_sub *s);
VkDeviceMemory nvn_vk_mem_handle(const struct vkmem_sub *s); VkDeviceSize nvn_vk_mem_offset(const struct vkmem_sub *s);
void nvn_vk_defer_image_sub(VkImage img, VkImageView view, struct vkmem_sub *sub);          // destroy once the GPU has passed the submission being recorded, then give the memory back
int nvn_vk_oneshot_submit(VkCommandBuffer cb, VkFence *fence);   // submit a one-shot command buffer without waiting for it
int nvn_vk_oneshot_wait(VkFence fence);                          // 0 when it has completed
void nvn_vk_oneshot_release(VkCommandBuffer cb, VkFence fence);  // free the command buffer and the fence once it has
void nvn_vk_barrier_all(VkCommandBuffer cb);   // immediate full barrier (used by OP_BARRIER and OP_UPDATE_UB)
void nvn_vk_barrier_mark(void);                // deferred mark: a full barrier is needed before the next read
void nvn_vk_barrier_flush(VkCommandBuffer cb);  // emits one full barrier if a deferred one is pending (many marks collapse into a single)
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
// Clears of a render target in the submission being recorded (nvn_tex_vk.c). rect = x, y, w, h of the scissor, or NULL for the whole level.
// A colour clear takes rgba as the game recorded it (floats; converted to the attachment's integer type for integer formats).
void nvn_vk_clear_color(uint64_t serial, const view_t *v, int has_view, int mask, const float rgba[4], const int *rect);
void nvn_vk_clear_ds(uint64_t serial, const view_t *v, int has_view, float depth, int depth_mask, int stencil, int stencil_mask, const int *rect);

// The draw path (nvn_draw_vk.c, nvn_tex_vk.c)
typedef struct { VkImageView view; VkFormat fmt; uint32_t w, h, layers; VkImageAspectFlags aspect; } nvn_attach;
int nvn_vk_tex_attach(uint64_t serial, const view_t *v, int has_view, nvn_attach *out);
// A sampling view of a texture (it fills a dirty image first, which ends the render pass), and the SamplerType it presents (nvn_tex_vk.c)
// Texture and sampler descriptors registered by the game (nvn_desc_vk.c): a bound handle (texture id | sampler id << 20) as a descriptor for a binding that samples SamplerType `want`.
// 1 = filled (an unregistered id reads as a 1x1 texture of zeros); 0 = no usable texture and no stand-in; 3 = the texture is of another type
int nvn_vk_desc_sampled(uint32_t handle, int want, VkDescriptorImageInfo *out);
int nvn_vk_tex_sampled(uint64_t serial, const view_t *v, int has_view, int tex_set_swz, unsigned tex_swz, VkImageView *out, int *code);
VkCommandBuffer nvn_vk_cmd_raw(void);       // the submission's command buffer without ending an open render pass: for the draw path only
