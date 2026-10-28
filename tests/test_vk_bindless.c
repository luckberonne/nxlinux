// Bindless textures the NVN way: a large partially-bound, update-after-bind array of sampled images and one of samplers,
// indexed dynamically from a handle read through a buffer device address.
#include "../src/gfx/vk/vkctx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)
#define NTEX 1024
#define NSMP 16

static vkctx_t C;

// Uploads w*h RGBA8 texels and leaves the image in SHADER_READ_ONLY_OPTIMAL
static int make_texture(vkimg_t *img, uint32_t w, uint32_t h, const uint32_t *texels) {
    if (vkimg_create2d(&C, img, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1)) return -1;
    vkbuf_t st; if (vkbuf_create(&C, &st, w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1)) return -1;
    memcpy(st.map, texels, w * h * 4);
    VkCommandBuffer cb = vkctx_begin(&C);
    vk_barrier_image(cb, img->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, st.buf, img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    vk_barrier_image(cb, img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    int r = vkctx_end(&C, cb);
    vkbuf_destroy(&C, &st);
    return r;
}

static void write_tex(VkDescriptorSet set, uint32_t slot, VkImageView v) {
    VkDescriptorImageInfo ii = {.imageView = v, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .dstArrayElement = slot, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii};
    vkUpdateDescriptorSets(C.dev, 1, &w, 0, NULL);
}

static uint32_t pack(float r, float g, float b) { return (uint32_t)(r * 255) | (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255) << 16 | 0xFF000000u; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s bindless.comp.spv\n", argv[0]); return 2; }
    if (vkctx_create(&C)) return 1;
    printf("device: %s\n", C.props.deviceName);

    vkimg_t red, green, blue, split, yellow;
    uint32_t r4[16], g4[16], b4[16], y4[16], rb[2] = {pack(1, 0, 0), pack(0, 0, 1)};
    for (int i = 0; i < 16; i++) { r4[i] = pack(1, 0, 0); g4[i] = pack(0, 1, 0); b4[i] = pack(0, 0, 1); y4[i] = pack(1, 1, 0); }
    CHECK(!make_texture(&red, 4, 4, r4) && !make_texture(&green, 4, 4, g4) && !make_texture(&blue, 4, 4, b4));
    CHECK(!make_texture(&split, 2, 1, rb) && !make_texture(&yellow, 4, 4, y4));

    // Samplers: 0 nearest, 1 linear
    VkSampler smp[2];
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(!vkCreateSampler(C.dev, &sci, NULL, &smp[0]));
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    CHECK(!vkCreateSampler(C.dev, &sci, NULL, &smp[1]));

    // Descriptor set: two unbounded-style arrays, partially bound and updatable after the set is bound
    VkDescriptorBindingFlags bf[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                                      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 2, .pBindingFlags = bf};
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NTEX, VK_SHADER_STAGE_COMPUTE_BIT}, {1, VK_DESCRIPTOR_TYPE_SAMPLER, NSMP, VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 2, lb};
    VkDescriptorSetLayout dsl; CHECK(!vkCreateDescriptorSetLayout(C.dev, &dli, NULL, &dsl));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NTEX}, {VK_DESCRIPTOR_TYPE_SAMPLER, NSMP}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps};
    VkDescriptorPool dp; CHECK(!vkCreateDescriptorPool(C.dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet set; CHECK(!vkAllocateDescriptorSets(C.dev, &dai, &set));

    write_tex(set, 0, red.view); write_tex(set, 1, green.view); write_tex(set, 5, blue.view); write_tex(set, 3, split.view);   // slots 2, 4, 6, 7... stay unwritten
    for (uint32_t i = 0; i < 2; i++) {
        VkDescriptorImageInfo ii = {.sampler = smp[i]};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .dstArrayElement = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii};
        vkUpdateDescriptorSets(C.dev, 1, &w, 0, NULL);
    }

    VkShaderModule sm = vkctx_shader_from_file(&C, argv[1]); CHECK(sm);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout; CHECK(!vkCreatePipelineLayout(C.dev, &pli, NULL, &layout));
    VkComputePipelineCreateInfo pi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
    VkPipeline pipe; CHECK(!vkCreateComputePipelines(C.dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipe));

    vkbuf_t hb, ob;
    CHECK(!vkbuf_create(&C, &hb, 64, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1));
    CHECK(!vkbuf_create(&C, &ob, 8 * 16, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1));
    uint32_t *handles = hb.map;
    handles[0] = 0 | 0u << 20;      // red, nearest
    handles[1] = 1 | 0u << 20;      // green, nearest
    handles[2] = 5 | 0u << 20;      // blue, nearest
    handles[3] = 3 | 1u << 20;      // red|blue split, linear: a blend
    handles[4] = 1 | 1u << 20;      // green, linear
    handles[5] = 7 | 0u << 20;      // yellow: written to the set only after the command buffer was recorded
    handles[6] = 0 | 1u << 20;      // red, linear
    handles[7] = 5 | 1u << 20;      // blue, linear
    struct { VkDeviceAddress dst, handles; uint32_t n, pad; } pc = {ob.addr, hb.addr, 8, 0};

    VkCommandBuffer cb = vkctx_begin(&C);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 24, &pc);
    vkCmdDispatch(cb, 1, 1, 1);
    write_tex(set, 7, yellow.view);                       // update after bind: the set is already bound in the recorded command buffer
    CHECK(!vkctx_end(&C, cb));

    const float (*o)[4] = ob.map;
    for (int i = 0; i < 8; i++) printf("  handle %d -> (%.2f, %.2f, %.2f)\n", i, o[i][0], o[i][1], o[i][2]);
    CHECK(o[0][0] == 1 && o[0][1] == 0 && o[0][2] == 0);
    CHECK(o[1][0] == 0 && o[1][1] == 1 && o[1][2] == 0);
    CHECK(o[2][0] == 0 && o[2][1] == 0 && o[2][2] == 1);
    CHECK(fabsf(o[3][0] - 0.5f) < 0.02f && fabsf(o[3][2] - 0.5f) < 0.02f && o[3][1] == 0);      // linear across the red|blue seam
    CHECK(o[4][1] == 1 && o[4][0] == 0);
    CHECK(o[5][0] == 1 && o[5][1] == 1 && o[5][2] == 0);                                        // the descriptor written after binding
    CHECK(o[6][0] == 1 && o[7][2] == 1);

    vkbuf_destroy(&C, &hb); vkbuf_destroy(&C, &ob);
    vkDestroyPipeline(C.dev, pipe, NULL); vkDestroyPipelineLayout(C.dev, layout, NULL); vkDestroyShaderModule(C.dev, sm, NULL);
    vkDestroyDescriptorPool(C.dev, dp, NULL); vkDestroyDescriptorSetLayout(C.dev, dsl, NULL);
    vkDestroySampler(C.dev, smp[0], NULL); vkDestroySampler(C.dev, smp[1], NULL);
    vkimg_destroy(&C, &red); vkimg_destroy(&C, &green); vkimg_destroy(&C, &blue); vkimg_destroy(&C, &split); vkimg_destroy(&C, &yellow);
    vkctx_destroy(&C);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
