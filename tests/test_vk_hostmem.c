// Zero-copy memory: a range of the guest window is imported as Vulkan memory (VK_EXT_external_memory_host). The CPU writes through its
// pointer and the GPU reads it; the GPU writes through a buffer device address and the CPU sees it in the same pointer.
#include "../src/gfx/vk/vkctx.h"
#include "../src/host/guest_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s fill.comp.spv\n", argv[0]); return 2; }
    if (guest_mem_init(64ull << 20)) return 1;
    vkctx_t c;
    if (vkctx_create(&c)) return 1;
    if (!c.has_ext_mem_host) { puts("SKIP: VK_EXT_external_memory_host is not available"); return 0; }
    printf("device: %s, import alignment %llu\n", c.props.deviceName, (unsigned long long)c.host_import_align);

    const VkDeviceSize size = 1 << 20;                                   // a 1 MiB "memory pool" inside the guest window
    uint32_t *pool = (uint32_t *)guest_alloc(size, c.host_import_align);
    CHECK(pool && ((uintptr_t)pool % c.host_import_align) == 0);
    for (uint32_t i = 0; i < size / 4; i++) pool[i] = 0xC0DE0000u + i;     // CPU writes first

    // Import the guest range as device memory
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(c.GetMemoryHostPointerProperties(c.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pool, &hp) == VK_SUCCESS);
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, .size = size, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VkBuffer buf; CHECK(!vkCreateBuffer(c.dev, &bi, NULL, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c.dev, buf, &mr);
    uint32_t bits = mr.memoryTypeBits & hp.memoryTypeBits;
    uint32_t mt = vkctx_memtype(&c, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(mt != UINT32_MAX);
    printf("imported as memory type %u (host-pointer types %#x, buffer types %#x)\n", mt, hp.memoryTypeBits, mr.memoryTypeBits);
    VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, &fi, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pool};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, mt};
    VkDeviceMemory mem; VkResult r = vkAllocateMemory(c.dev, &ai, NULL, &mem);
    CHECK(r == VK_SUCCESS);
    CHECK(!vkBindBufferMemory(c.dev, buf, mem, 0));
    VkBufferDeviceAddressInfo di = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
    VkDeviceAddress gpu_addr = vkGetBufferDeviceAddress(c.dev, &di);
    printf("guest %p  ->  GPU address %#llx\n", (void *)pool, (unsigned long long)gpu_addr);

    // 1. CPU -> GPU: copy the pool into a separate buffer and read that back
    vkbuf_t rb; CHECK(!vkbuf_create(&c, &rb, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1));
    VkCommandBuffer cb = vkctx_begin(&c);
    VkBufferCopy cp = {.srcOffset = 4096, .dstOffset = 0, .size = 4096};
    vkCmdCopyBuffer(cb, buf, rb.buf, 1, &cp);
    CHECK(!vkctx_end(&c, cb));
    int ok1 = 1; for (uint32_t i = 0; i < 1024; i++) if (((uint32_t *)rb.map)[i] != 0xC0DE0000u + 1024 + i) ok1 = 0;
    printf("CPU->GPU copy of imported memory: %s\n", ok1 ? "ok" : "MISMATCH");
    CHECK(ok1);

    // 2. GPU -> CPU through a transfer: fill 4 KiB in the middle of the pool
    cb = vkctx_begin(&c);
    vkCmdFillBuffer(cb, buf, 8192, 4096, 0xDEADBEEFu);
    CHECK(!vkctx_end(&c, cb));
    int ok2 = 1; for (uint32_t i = 0; i < 1024; i++) if (pool[2048 + i] != 0xDEADBEEFu) ok2 = 0;
    CHECK(pool[2047] == 0xC0DE0000u + 2047 && pool[3072] == 0xC0DE0000u + 3072);      // the neighbours are untouched
    printf("GPU fill seen through the CPU pointer: %s\n", ok2 ? "ok" : "MISMATCH");
    CHECK(ok2);

    // 3. a compute shader writes through the raw GPU address
    VkShaderModule sm = vkctx_shader_from_file(&c, argv[1]); CHECK(sm);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout; CHECK(!vkCreatePipelineLayout(c.dev, &li, NULL, &layout));
    VkComputePipelineCreateInfo pi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"}};
    VkPipeline pipe; CHECK(!vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipe));
    struct { VkDeviceAddress dst; uint32_t n, pad; } pc = {gpu_addr + 65536, 1000, 0};       // 64 KiB into the pool
    cb = vkctx_begin(&c);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (pc.n + 63) / 64, 1, 1);
    CHECK(!vkctx_end(&c, cb));
    int ok3 = 1; for (uint32_t i = 0; i < 1000; i++) if (pool[16384 + i] != i * 3u + 7u) ok3 = 0;
    CHECK(pool[16384 + 1000] == 0xC0DE0000u + 16384 + 1000);                               // past n: untouched
    printf("compute write by GPU address seen through the CPU pointer: %s\n", ok3 ? "ok" : "MISMATCH");
    CHECK(ok3);

    vkDestroyPipeline(c.dev, pipe, NULL); vkDestroyPipelineLayout(c.dev, layout, NULL); vkDestroyShaderModule(c.dev, sm, NULL);
    vkbuf_destroy(&c, &rb); vkDestroyBuffer(c.dev, buf, NULL); vkFreeMemory(c.dev, mem, NULL);
    vkctx_destroy(&c);
    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
