// Headless render: vertices fetched through a buffer device address, dynamic rendering into an RGBA8 image, readback.
#include "../src/gfx/vk/vkctx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static VkPipeline make_pipeline(vkctx_t *c, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs) {
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, &p)) fprintf(stderr, "pipeline creation failed\n");
    return p;
}

// Renders three vertices (pos, color pairs) over a blue clear and returns the pixels in out[W*H*4]
static int render(vkctx_t *c, VkPipeline pipe, VkPipelineLayout layout, const float verts[24], uint8_t *out) {
    vkbuf_t vb, rb; vkimg_t img;
    if (vkbuf_create(c, &vb, 24 * 4, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1)) return -1;
    if (vkbuf_create(c, &rb, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) return -1;
    if (vkimg_create2d(c, &img, VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1)) return -1;
    memcpy(vb.map, verts, 24 * 4);

    VkCommandBuffer cb = vkctx_begin(c);
    vk_barrier_image(cb, img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = img.view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {{0.f, 0.f, 1.f, 1.f}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = {0, 0, W, H, 0, 1}; VkRect2D sc = {{0, 0}, {W, H}};
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, &vb.addr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_barrier_image(cb, img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &bc);
    int r = vkctx_end(c, cb);
    if (!r) memcpy(out, rb.map, W * H * 4);
    vkimg_destroy(c, &img); vkbuf_destroy(c, &rb); vkbuf_destroy(c, &vb);
    return r;
}

static const uint8_t *px(const uint8_t *p, int x, int y) { return p + (y * W + x) * 4; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s tri.vert.spv tri.frag.spv\n", argv[0]); return 2; }
    vkctx_t c;
    if (vkctx_create(&c)) return 1;
    printf("device: %s%s\n", c.props.deviceName, c.validation ? " (validation on)" : "");
    VkShaderModule vs = vkctx_shader_from_file(&c, argv[1]), fs = vkctx_shader_from_file(&c, argv[2]);
    CHECK(vs && fs);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout; CHECK(!vkCreatePipelineLayout(c.dev, &li, NULL, &layout));
    VkPipeline pipe = make_pipeline(&c, layout, vs, fs);
    CHECK(pipe);

    uint8_t *pix = malloc(W * H * 4);
    // 1. a solid red triangle over a blue clear
    const float solid[24] = {-0.8f, -0.8f, 0, 1, 1, 0, 0, 1,    0.8f, -0.8f, 0, 1, 1, 0, 0, 1,    0.0f, 0.8f, 0, 1, 1, 0, 0, 1};
    CHECK(!render(&c, pipe, layout, solid, pix));
    const uint8_t *center = px(pix, 32, 32), *corner = px(pix, 1, 1), *corner2 = px(pix, 62, 1);
    printf("solid: center=(%u,%u,%u) corner=(%u,%u,%u)\n", center[0], center[1], center[2], corner[0], corner[1], corner[2]);
    CHECK(center[0] == 255 && center[1] == 0 && center[2] == 0);          // inside: red
    CHECK(corner[0] == 0 && corner[1] == 0 && corner[2] == 255);          // outside: the blue clear
    CHECK(corner2[0] == 0 && corner2[2] == 255);

    // 2. vertex colors interpolate: red, green, blue corners give (0.25R + 0.25G + 0.5B) at the centroid-ish middle
    const float grad[24] = {-0.8f, -0.8f, 0, 1, 1, 0, 0, 1,    0.8f, -0.8f, 0, 1, 0, 1, 0, 1,    0.0f, 0.8f, 0, 1, 0, 0, 1, 1};
    CHECK(!render(&c, pipe, layout, grad, pix));
    center = px(pix, 32, 32);
    printf("gradient: center=(%u,%u,%u)\n", center[0], center[1], center[2]);
    CHECK(abs((int)center[0] - 64) <= 8 && abs((int)center[1] - 64) <= 8 && abs((int)center[2] - 128) <= 8);

    vkDestroyPipeline(c.dev, pipe, NULL); vkDestroyPipelineLayout(c.dev, layout, NULL);
    vkDestroyShaderModule(c.dev, vs, NULL); vkDestroyShaderModule(c.dev, fs, NULL);
    free(pix); vkctx_destroy(&c);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
