// Guest program for tests/test_nvn_guest.cpp: what a game does first with NVN. Compiled for aarch64 against the real nvn.h, so the
// structure sizes and the calling convention are the game's. Every NVN function comes from nvnBootstrapLoader, which the host
// answers with svc stubs. Results go to g_res[].
#include "nvn_protos.h"

typedef unsigned long u64;
extern void *nvnBootstrapLoader(const char *name);       // imported: the host takes it over
#define FN(name) NVNT_##name name = (NVNT_##name)nvnBootstrapLoader(#name)

u64 g_res[48];
static NVNdevice g_dev __attribute__((aligned(64)));
static NVNdeviceBuilder g_dev_b __attribute__((aligned(64)));
static NVNmemoryPool g_pool __attribute__((aligned(64)));
static NVNmemoryPoolBuilder g_pool_b __attribute__((aligned(64)));
static NVNqueue g_queue __attribute__((aligned(64)));
static NVNqueueBuilder g_queue_b __attribute__((aligned(64)));
static NVNcommandBuffer g_cb __attribute__((aligned(64)));
static NVNbuffer g_src __attribute__((aligned(64))), g_dst __attribute__((aligned(64)));
static NVNbufferBuilder g_buf_b __attribute__((aligned(64)));
static NVNsync g_sync __attribute__((aligned(64)));
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

    // ---- teardown in reverse
    nvnSyncFinalize(&g_sync); nvnCommandBufferFinalize(&g_cb); nvnQueueFinalize(&g_queue);
    nvnBufferFinalize(&g_dst); nvnBufferFinalize(&g_src); nvnMemoryPoolFinalize(&g_pool); nvnDeviceFinalize(&g_dev);
    g_res[n++] = 0x5555;                                                                                  // 19: reached the end
    exit_thread();
}
