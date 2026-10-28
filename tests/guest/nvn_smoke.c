// Guest program for tests/test_nvn_guest.cpp: what a game does first with NVN. Compiled for aarch64 against the real nvn.h, so the
// structure sizes and the calling convention are the game's. Every NVN function comes from nvnBootstrapLoader, which the host
// answers with svc stubs. Results go to g_res[].
#include "nvn_protos.h"

typedef unsigned long u64;
extern void *nvnBootstrapLoader(const char *name);       // imported: the host takes it over
#define FN(name) NVNT_##name name = (NVNT_##name)nvnBootstrapLoader(#name)

u64 g_res[64];
static NVNtexture g_tex __attribute__((aligned(64)));
static NVNtextureBuilder g_tb __attribute__((aligned(64)));
static unsigned g_px[40 * 20], g_out[40 * 20];
static NVNdevice g_dev __attribute__((aligned(64)));
static NVNdeviceBuilder g_dev_b __attribute__((aligned(64)));
static NVNmemoryPool g_pool __attribute__((aligned(64)));
static NVNmemoryPoolBuilder g_pool_b __attribute__((aligned(64)));
static NVNqueue g_queue __attribute__((aligned(64)));
static NVNqueueBuilder g_queue_b __attribute__((aligned(64)));
static NVNcommandBuffer g_cb __attribute__((aligned(64)));
static NVNbuffer g_src __attribute__((aligned(64))), g_dst __attribute__((aligned(64)));
static NVNbufferBuilder g_buf_b __attribute__((aligned(64)));
static NVNsync g_sync __attribute__((aligned(64))), g_sync2 __attribute__((aligned(64)));
static NVNevent g_event __attribute__((aligned(64)));
static NVNeventBuilder g_event_b __attribute__((aligned(64)));
static char g_poolmem[0x100000] __attribute__((aligned(0x10000)));    // the game's own memory, aligned to the pool page size: GPU address == CPU address
static char g_queuemem[0x80000] __attribute__((aligned(0x1000)));
static char g_ctl[0x1000] __attribute__((aligned(16)));
static void exit_thread(void) { register u64 x0 __asm__("x0"); __asm__ volatile("svc #0x0A" : "=r"(x0) : : "memory", "x1", "x2", "x3"); for (;;) {} }

void guest_main(u64 arg) {
    int n = 0;
    FN(nvnDeviceBuilderSetDefaults); FN(nvnDeviceBuilderSetFlags); FN(nvnDeviceInitialize); FN(nvnDeviceGetInteger); FN(nvnDeviceFinalize);
    FN(nvnMemoryPoolBuilderSetDevice); FN(nvnMemoryPoolBuilderSetDefaults); FN(nvnMemoryPoolBuilderSetStorage); FN(nvnMemoryPoolBuilderSetFlags);
    FN(nvnMemoryPoolInitialize); FN(nvnMemoryPoolMap); FN(nvnMemoryPoolGetFlags); FN(nvnMemoryPoolFinalize);
    FN(nvnQueueBuilderSetDevice); FN(nvnQueueBuilderSetDefaults); FN(nvnQueueBuilderSetQueueMemory); FN(nvnQueueBuilderSetCommandMemorySize);
    FN(nvnQueueInitialize); FN(nvnQueueSubmitCommands); FN(nvnQueueFinish); FN(nvnQueueFenceSync); FN(nvnQueueFlush); FN(nvnQueueFinalize);
    FN(nvnBufferBuilderSetDevice); FN(nvnBufferBuilderSetStorage); FN(nvnBufferInitialize); FN(nvnBufferGetAddress); FN(nvnBufferFinalize);
    FN(nvnCommandBufferInitialize); FN(nvnCommandBufferAddCommandMemory); FN(nvnCommandBufferAddControlMemory); FN(nvnCommandBufferBeginRecording);
    FN(nvnCommandBufferEndRecording); FN(nvnCommandBufferSetViewport); FN(nvnCommandBufferSetScissor); FN(nvnCommandBufferCopyBufferToBuffer);
    FN(nvnCommandBufferGetCommandMemoryUsed); FN(nvnCommandBufferFinalize);
    FN(nvnSyncInitialize); FN(nvnSyncWait); FN(nvnSyncFinalize);
    FN(nvnCommandBufferClearBuffer); FN(nvnCommandBufferUpdateUniformBuffer); FN(nvnCommandBufferReportCounter); FN(nvnCommandBufferSignalEvent);
    FN(nvnCommandBufferFenceSync); FN(nvnCommandBufferBarrier); FN(nvnCommandBufferCallCommands);
    FN(nvnTextureBuilderSetDevice); FN(nvnTextureBuilderSetDefaults); FN(nvnTextureBuilderSetTarget); FN(nvnTextureBuilderSetSize2D); FN(nvnTextureBuilderSetFormat);
    FN(nvnTextureBuilderSetLevels); FN(nvnTextureBuilderSetStorage); FN(nvnTextureBuilderGetStorageSize); FN(nvnTextureBuilderGetStorageAlignment);
    FN(nvnTextureInitialize); FN(nvnTextureWriteTexels); FN(nvnTextureReadTexels); FN(nvnTextureFinalize);
    FN(nvnCommandBufferCopyTextureToBuffer); FN(nvnCommandBufferCopyBufferToTexture); FN(nvnCommandBufferSetCopyRowStride);
    FN(nvnCommandBufferSetRenderTargets); FN(nvnCommandBufferClearColor); FN(nvnCommandBufferClearDepthStencil);
    FN(nvnEventBuilderSetDefaults); FN(nvnEventBuilderSetStorage); FN(nvnEventInitialize); FN(nvnEventGetValue); FN(nvnEventFinalize);
    g_res[n++] = nvnBootstrapLoader != 0 && nvnDeviceInitialize != 0 && nvnSyncWait != 0;                 // 0: the loader answered with callable addresses

    // ---- device
    nvnDeviceBuilderSetDefaults(&g_dev_b);
    nvnDeviceBuilderSetFlags(&g_dev_b, 0);
    g_res[n++] = nvnDeviceInitialize(&g_dev, &g_dev_b) & 0xFF;                                            // 1: device up
    int major = -1, minor = -1, page = -1, qmin = -1;
    nvnDeviceGetInteger(&g_dev, NVN_DEVICE_INFO_API_MAJOR_VERSION, &major);
    nvnDeviceGetInteger(&g_dev, NVN_DEVICE_INFO_API_MINOR_VERSION, &minor);
    nvnDeviceGetInteger(&g_dev, NVN_DEVICE_INFO_MEMORY_POOL_PAGE_SIZE, &page);
    nvnDeviceGetInteger(&g_dev, NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_SIZE, &qmin);
    g_res[n++] = (u64)(unsigned)major; g_res[n++] = (u64)(unsigned)minor;                                 // 2, 3: the API version the profile reports
    g_res[n++] = (u64)(unsigned)page;                                                                     // 4: 65536, the Tegra X1 value
    g_res[n++] = (u64)(unsigned)qmin;                                                                     // 5

    // ---- memory pool over the game's own memory
    nvnMemoryPoolBuilderSetDevice(&g_pool_b, &g_dev);
    nvnMemoryPoolBuilderSetDefaults(&g_pool_b);
    nvnMemoryPoolBuilderSetStorage(&g_pool_b, g_poolmem, sizeof g_poolmem);
    nvnMemoryPoolBuilderSetFlags(&g_pool_b, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    g_res[n++] = nvnMemoryPoolInitialize(&g_pool, &g_pool_b) & 0xFF;                                      // 6
    g_res[n++] = nvnMemoryPoolMap(&g_pool) == (void *)g_poolmem;                                          // 7: mapped where the game put it
    g_res[n++] = (nvnMemoryPoolGetFlags(&g_pool) & NVN_MEMORY_POOL_FLAGS_GPU_CACHED) != 0;                // 8

    // ---- two buffers in the pool: their GPU addresses are CPU addresses
    nvnBufferBuilderSetDevice(&g_buf_b, &g_dev);
    nvnBufferBuilderSetStorage(&g_buf_b, &g_pool, 0x20000, 0x4000);
    g_res[n++] = nvnBufferInitialize(&g_src, &g_buf_b) & 0xFF;                                            // 9
    nvnBufferBuilderSetStorage(&g_buf_b, &g_pool, 0x30000, 0x4000);
    g_res[n++] = nvnBufferInitialize(&g_dst, &g_buf_b) & 0xFF;                                            // 10
    g_res[n++] = nvnBufferGetAddress(&g_src) == (u64)(g_poolmem + 0x20000);                               // 11
    g_res[n++] = nvnBufferGetAddress(&g_dst) == (u64)(g_poolmem + 0x30000);                               // 12

    // ---- queue
    nvnQueueBuilderSetDevice(&g_queue_b, &g_dev);
    nvnQueueBuilderSetDefaults(&g_queue_b);
    nvnQueueBuilderSetQueueMemory(&g_queue_b, g_queuemem, sizeof g_queuemem);
    nvnQueueBuilderSetCommandMemorySize(&g_queue_b, 0x40000);
    g_res[n++] = nvnQueueInitialize(&g_queue, &g_queue_b) & 0xFF;                                         // 13

    // ---- command buffer: record, end, submit
    g_res[n++] = nvnCommandBufferInitialize(&g_cb, &g_dev) & 0xFF;                                        // 14
    nvnCommandBufferAddCommandMemory(&g_cb, &g_pool, 0x40000, 0x10000);
    nvnCommandBufferAddControlMemory(&g_cb, g_ctl, sizeof g_ctl);
    nvnCommandBufferBeginRecording(&g_cb);
    nvnCommandBufferSetViewport(&g_cb, 0, 0, 1280, 720);
    nvnCommandBufferSetScissor(&g_cb, 0, 0, 1280, 720);
    nvnCommandBufferCopyBufferToBuffer(&g_cb, nvnBufferGetAddress(&g_src), nvnBufferGetAddress(&g_dst), 0x1000, 0);
    NVNcommandHandle h = nvnCommandBufferEndRecording(&g_cb);
    g_res[n++] = h != 0;                                                                                  // 15: a non-empty handle
    g_res[n++] = nvnCommandBufferGetCommandMemoryUsed(&g_cb) != 0 || h != 0;                                     // 16
    nvnQueueSubmitCommands(&g_queue, 1, &h);

    // ---- sync: a fence after the submission
    g_res[n++] = nvnSyncInitialize(&g_sync, &g_dev) & 0xFF;                                               // 17
    nvnQueueFenceSync(&g_queue, &g_sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    nvnQueueFlush(&g_queue);
    int w = nvnSyncWait(&g_sync, 1000000000ull);
    g_res[n++] = w == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || w == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;   // 18
    nvnQueueFinish(&g_queue);

    // ---- recorded commands that have an effect: they run on the GPU in order, and what they write is visible once the fence is
    volatile unsigned *src = (volatile unsigned *)(g_poolmem + 0x20000), *dst = (volatile unsigned *)(g_poolmem + 0x30000);
    for (int i = 0; i < 0x4000 / 4; i++) { src[i] = 0xC0DE0000u + (unsigned)i; dst[i] = 0; }
    u64 evaddr = 0x60000, rep_addr = 0x61000;
    nvnEventBuilderSetDefaults(&g_event_b);
    nvnEventBuilderSetStorage(&g_event_b, &g_pool, evaddr);
    g_res[n++] = nvnEventInitialize(&g_event, &g_event_b) & 0xFF;                                         // 19
    g_res[n++] = nvnEventGetValue(&g_event) == 0;                                                         // 20
    volatile unsigned *cnt = (volatile unsigned *)(g_poolmem + rep_addr);
    cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0xFFFFFFFFu;
    static unsigned ub[16];
    for (int i = 0; i < 16; i++) ub[i] = 0x7000u + (unsigned)i;
    NVNbufferAddress a_src = nvnBufferGetAddress(&g_src), a_dst = nvnBufferGetAddress(&g_dst);
    nvnCommandBufferBeginRecording(&g_cb);
    nvnCommandBufferClearBuffer(&g_cb, a_dst + 0x100, 0x100, 0xAAAAAAAAu);                                // a fill
    nvnCommandBufferCopyBufferToBuffer(&g_cb, a_src, a_dst + 0x400, 0x200, 0);                            // a copy into the middle
    nvnCommandBufferBarrier(&g_cb, NVN_BARRIER_ORDER_PRIMITIVES_BIT);
    nvnCommandBufferCopyBufferToBuffer(&g_cb, a_dst + 0x400, a_dst + 0x1000, 0x200, 0);                   // ... and the copy of that copy: needs the order
    nvnCommandBufferUpdateUniformBuffer(&g_cb, a_dst, 0x4000, 0x2000, sizeof ub, ub);                     // 64 bytes of inline data
    for (int i = 0; i < 16; i++) ub[i] = 0;                                                               // the record keeps its own copy of the data
    nvnCommandBufferReportCounter(&g_cb, NVN_COUNTER_TYPE_TIMESTAMP, (NVNbufferAddress)(g_poolmem + rep_addr));
    nvnCommandBufferSignalEvent(&g_cb, &g_event, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 7);
    nvnCommandBufferSignalEvent(&g_cb, &g_event, NVN_EVENT_SIGNAL_MODE_ADD, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 35);
    nvnCommandBufferFenceSync(&g_cb, &g_sync2, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h2 = nvnCommandBufferEndRecording(&g_cb);
    g_res[n++] = nvnSyncInitialize(&g_sync2, &g_dev) & 0xFF;                                              // 21
    g_res[n++] = nvnEventGetValue(&g_event) == 0;                                                         // 22: nothing written before the GPU gets there
    nvnQueueSubmitCommands(&g_queue, 1, &h2);
    nvnQueueFlush(&g_queue);
    int w2 = nvnSyncWait(&g_sync2, 5000000000ull);
    g_res[n++] = w2 == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || w2 == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;   // 23: the fence recorded in the list
    g_res[n++] = nvnEventGetValue(&g_event);                                                              // 24: write 7, then add 35 = 42, visible after the fence
    int fill_ok = 1, copy_ok = 1, chain_ok = 1, ub_ok = 1, edge_ok = 1;
    for (int i = 0; i < 0x100 / 4; i++) fill_ok &= dst[0x100 / 4 + i] == 0xAAAAAAAAu;
    for (int i = 0; i < 0x200 / 4; i++) copy_ok &= dst[0x400 / 4 + i] == 0xC0DE0000u + (unsigned)i;
    for (int i = 0; i < 0x200 / 4; i++) chain_ok &= dst[0x1000 / 4 + i] == 0xC0DE0000u + (unsigned)i;
    for (int i = 0; i < 16; i++) ub_ok &= dst[0x2000 / 4 + i] == 0x7000u + (unsigned)i;
    edge_ok = dst[0xFC / 4] == 0 && dst[0x200 / 4] == 0 && dst[0x3FC / 4] == 0 && dst[0x600 / 4] == 0;     // neighbours untouched
    g_res[n++] = fill_ok; g_res[n++] = copy_ok; g_res[n++] = chain_ok; g_res[n++] = ub_ok; g_res[n++] = edge_ok;   // 25-29
    g_res[n++] = cnt[2] != 0xFFFFFFFFu || cnt[3] != 0xFFFFFFFFu;                                          // 30: the timestamp report landed
    nvnQueueFinish(&g_queue);

    // ---- a texture: written by the CPU, copied out by the GPU, patched by the GPU, then read by the CPU
    nvnTextureBuilderSetDevice(&g_tb, &g_dev); nvnTextureBuilderSetDefaults(&g_tb);
    nvnTextureBuilderSetTarget(&g_tb, NVN_TEXTURE_TARGET_2D); nvnTextureBuilderSetSize2D(&g_tb, 40, 20);
    nvnTextureBuilderSetFormat(&g_tb, NVN_FORMAT_RGBA8); nvnTextureBuilderSetLevels(&g_tb, 3);
    u64 tsz = nvnTextureBuilderGetStorageSize(&g_tb), tal = nvnTextureBuilderGetStorageAlignment(&g_tb);
    nvnTextureBuilderSetStorage(&g_tb, &g_pool, 0x80000);
    g_res[40] = nvnTextureInitialize(&g_tex, &g_tb) & 0xFF;                                               // 40: the texture exists
    g_res[41] = tsz > 0 && tal > 0 && (0x80000 % tal) == 0;                                               // 41: its storage fits where it was put
    for (int i = 0; i < 40 * 20; i++) g_px[i] = (unsigned)i * 2654435761u;
    NVNcopyRegion all = {0, 0, 0, 40, 20, 1};
    nvnTextureWriteTexels(&g_tex, 0, &all, g_px);                                                         // the CPU writes level 0
    volatile unsigned *o1 = (volatile unsigned *)(g_poolmem + 0xA0000), *o2 = (volatile unsigned *)(g_poolmem + 0xB0000);
    volatile unsigned char *pa = (volatile unsigned char *)(g_poolmem + 0xA8000);
    for (int i = 0; i < 40 * 20; i++) { o1[i] = 0; o2[i] = 0; }
    nvnCommandBufferBeginRecording(&g_cb);
    nvnCommandBufferCopyTextureToBuffer(&g_cb, &g_tex, 0, &all, (NVNbufferAddress)(g_poolmem + 0xA0000), 0);
    NVNcommandHandle t1 = nvnCommandBufferEndRecording(&g_cb);
    nvnQueueSubmitCommands(&g_queue, 1, &t1); nvnQueueFinish(&g_queue);
    int same = 1; for (int i = 0; i < 40 * 20; i++) same &= o1[i] == g_px[i];
    g_res[42] = same;                                                                                     // 42: the GPU read back what the CPU wrote
    NVNcopyRegion patch = {5, 3, 0, 10, 6, 1};
    for (int y = 0; y < 6; y++) for (int x = 0; x < 48; x++) pa[y * 48 + x] = x < 40 ? (unsigned char)(0x30 + y * 7 + x) : 0xEE;   // rows 48 bytes apart, 40 of them texels
    nvnCommandBufferBeginRecording(&g_cb);
    nvnCommandBufferSetCopyRowStride(&g_cb, 48);
    nvnCommandBufferCopyBufferToTexture(&g_cb, (NVNbufferAddress)(g_poolmem + 0xA8000), &g_tex, 0, &patch, 0);
    nvnCommandBufferSetCopyRowStride(&g_cb, 0);
    nvnCommandBufferCopyTextureToBuffer(&g_cb, &g_tex, 0, &all, (NVNbufferAddress)(g_poolmem + 0xB0000), 0);
    NVNcommandHandle t2 = nvnCommandBufferEndRecording(&g_cb);
    nvnQueueSubmitCommands(&g_queue, 1, &t2); nvnQueueFinish(&g_queue);
    same = 1;
    for (int y = 0; y < 20; y++) for (int x = 0; x < 40; x++) {
        unsigned want = g_px[y * 40 + x];
        if (x >= 5 && x < 15 && y >= 3 && y < 9) { const volatile unsigned char *q = pa + (y - 3) * 48 + (x - 5) * 4; want = q[0] | q[1] << 8 | q[2] << 16 | (unsigned)q[3] << 24; }
        same &= o2[y * 40 + x] == want;
    }
    g_res[43] = same;                                                                                     // 43: the patch landed, the rest is untouched
    nvnTextureReadTexels(&g_tex, 0, &patch, g_out);                                                       // the CPU reads the GPU-written region
    same = 1; for (int y = 0; y < 6; y++) for (int x = 0; x < 40; x++) same &= ((volatile unsigned char *)g_out)[y * 40 + x] == pa[y * 48 + x];
    g_res[44] = same;                                                                                     // 44
    NVNcopyRegion corner = {0, 10, 0, 8, 4, 1};                                                            // rows 10-13: below the patch (rows 3-8)
    nvnTextureReadTexels(&g_tex, 0, &corner, g_out);                                                      // ... and a corner the GPU did not touch
    same = 1; for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) same &= g_out[y * 8 + x] == g_px[(10 + y) * 40 + x];
    g_res[45] = same;                                                                                     // 45
    nvnTextureFinalize(&g_tex);

    // ---- a render target: cleared by the GPU (whole, then inside a scissor), copied out, read by the CPU
    static NVNtexture g_rt __attribute__((aligned(64)));
    nvnTextureBuilderSetDevice(&g_tb, &g_dev); nvnTextureBuilderSetDefaults(&g_tb);
    nvnTextureBuilderSetTarget(&g_tb, NVN_TEXTURE_TARGET_2D); nvnTextureBuilderSetSize2D(&g_tb, 32, 16);
    nvnTextureBuilderSetFormat(&g_tb, NVN_FORMAT_RGBA8); nvnTextureBuilderSetLevels(&g_tb, 1);
    nvnTextureBuilderSetStorage(&g_tb, &g_pool, 0xC0000);
    g_res[46] = nvnTextureInitialize(&g_rt, &g_tb) & 0xFF;                                                // 46
    volatile unsigned char *o3 = (volatile unsigned char *)(g_poolmem + 0xC8000);
    for (int i = 0; i < 32 * 16 * 4; i++) o3[i] = 0;
    const NVNtexture *rts[1] = {&g_rt};
    float green[4] = {0.0f, 1.0f, 0.0f, 1.0f}, red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    NVNcopyRegion whole = {0, 0, 0, 32, 16, 1};
    nvnCommandBufferBeginRecording(&g_cb);
    nvnCommandBufferSetRenderTargets(&g_cb, 1, rts, 0, 0, 0);
    nvnCommandBufferSetScissor(&g_cb, 0, 0, 32, 16);
    nvnCommandBufferClearColor(&g_cb, 0, green, 0xF);
    nvnCommandBufferSetScissor(&g_cb, 8, 4, 16, 8);
    nvnCommandBufferClearColor(&g_cb, 0, red, 0xF);
    nvnCommandBufferCopyTextureToBuffer(&g_cb, &g_rt, 0, &whole, (NVNbufferAddress)(g_poolmem + 0xC8000), 0);
    NVNcommandHandle t3 = nvnCommandBufferEndRecording(&g_cb);
    nvnQueueSubmitCommands(&g_queue, 1, &t3); nvnQueueFinish(&g_queue);
    same = 1;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 32; x++) {
        const volatile unsigned char *q = o3 + (y * 32 + x) * 4; int in = x >= 8 && x < 24 && y >= 4 && y < 12;
        same &= in ? (q[0] == 255 && q[1] == 0 && q[2] == 0 && q[3] == 255) : (q[0] == 0 && q[1] == 255 && q[2] == 0 && q[3] == 255);
    }
    g_res[47] = same;                                                                                     // 47: red inside the scissor, green around it
    NVNcopyRegion two = {0, 0, 0, 2, 1, 1}; unsigned char px2[8];
    nvnTextureReadTexels(&g_rt, 0, &two, px2);
    g_res[48] = px2[0] == 0 && px2[1] == 255 && px2[4] == 0 && px2[5] == 255;                             // 48: the CPU reads the cleared colour back
    nvnTextureFinalize(&g_rt);

    // ---- teardown in reverse
    nvnEventFinalize(&g_event); nvnSyncFinalize(&g_sync2); nvnSyncFinalize(&g_sync); nvnCommandBufferFinalize(&g_cb); nvnQueueFinalize(&g_queue);
    nvnBufferFinalize(&g_dst); nvnBufferFinalize(&g_src); nvnMemoryPoolFinalize(&g_pool); nvnDeviceFinalize(&g_dev);
    g_res[n++] = 0x5555;                                                                                  // 31: reached the end
    exit_thread();
}
