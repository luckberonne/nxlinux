// Guest program for tests/test_nvn_draw.cpp: draws through the NVN API, as a game does. The host registers SPIR-V for the two shaders the program names.
// The shader "code" is a few bytes in the memory pool and the control section a block in .bss; the host fills both in before the program runs and computes the
// key the way hle_nvn.c does. Six stages, each checking one thing; the results go to g_res[].
#include "nvn_protos.h"

typedef unsigned long u64;
typedef struct { float x, y, r, g, b, a; } V;
extern void *nvnBootstrapLoader(const char *name);

#define FUNCS(X) X(nvnDeviceBuilderSetDefaults) X(nvnDeviceBuilderSetFlags) X(nvnDeviceInitialize) \
    X(nvnMemoryPoolBuilderSetDevice) X(nvnMemoryPoolBuilderSetDefaults) X(nvnMemoryPoolBuilderSetStorage) X(nvnMemoryPoolBuilderSetFlags) X(nvnMemoryPoolInitialize) \
    X(nvnQueueBuilderSetDevice) X(nvnQueueBuilderSetDefaults) X(nvnQueueBuilderSetQueueMemory) X(nvnQueueBuilderSetCommandMemorySize) X(nvnQueueInitialize) \
    X(nvnQueueSubmitCommands) X(nvnQueueFinish) \
    X(nvnCommandBufferInitialize) X(nvnCommandBufferAddCommandMemory) X(nvnCommandBufferAddControlMemory) X(nvnCommandBufferBeginRecording) X(nvnCommandBufferEndRecording) \
    X(nvnCommandBufferSetViewport) X(nvnCommandBufferSetScissor) X(nvnCommandBufferSetRenderTargets) X(nvnCommandBufferClearColor) X(nvnCommandBufferClearDepthStencil) \
    X(nvnCommandBufferBindProgram) X(nvnCommandBufferBindBlendState) X(nvnCommandBufferBindColorState) X(nvnCommandBufferBindChannelMaskState) \
    X(nvnCommandBufferBindMultisampleState) X(nvnCommandBufferBindPolygonState) X(nvnCommandBufferBindDepthStencilState) \
    X(nvnCommandBufferBindVertexAttribState) X(nvnCommandBufferBindVertexStreamState) X(nvnCommandBufferBindVertexBuffer) X(nvnCommandBufferBindUniformBuffer) \
    X(nvnCommandBufferDrawArrays) X(nvnCommandBufferDrawElements) X(nvnCommandBufferCopyTextureToBuffer) \
    X(nvnTextureBuilderSetDevice) X(nvnTextureBuilderSetDefaults) X(nvnTextureBuilderSetTarget) X(nvnTextureBuilderSetSize2D) X(nvnTextureBuilderSetFormat) \
    X(nvnTextureBuilderSetLevels) X(nvnTextureBuilderSetStorage) X(nvnTextureInitialize) \
    X(nvnProgramInitialize) X(nvnProgramSetShaders) \
    X(nvnVertexAttribStateSetDefaults) X(nvnVertexAttribStateSetFormat) X(nvnVertexAttribStateSetStreamIndex) X(nvnVertexStreamStateSetDefaults) X(nvnVertexStreamStateSetStride) \
    X(nvnBlendStateSetDefaults) X(nvnBlendStateSetBlendTarget) X(nvnBlendStateSetBlendFunc) X(nvnColorStateSetDefaults) X(nvnColorStateSetBlendEnable) \
    X(nvnChannelMaskStateSetDefaults) X(nvnChannelMaskStateSetChannelMask) X(nvnMultisampleStateSetDefaults) \
    X(nvnPolygonStateSetDefaults) X(nvnPolygonStateSetCullFace) X(nvnPolygonStateSetFrontFace) \
    X(nvnDepthStencilStateSetDefaults) X(nvnDepthStencilStateSetDepthTestEnable) X(nvnDepthStencilStateSetDepthFunc) X(nvnDepthStencilStateSetDepthWriteEnable)
#define DECL(n) static NVNT_##n n;
#define LOAD(n) n = (NVNT_##n)nvnBootstrapLoader(#n);
FUNCS(DECL)

u64 g_res[64];
char g_vs_control[0x880] __attribute__((aligned(16))), g_fs_control[0x880] __attribute__((aligned(16))), g_x_control[0x880] __attribute__((aligned(16)));   /* g_x: a shader nobody registered */
char g_poolmem[0x100000] __attribute__((aligned(0x10000)));          // exported: the host writes the shader code into it before the program runs
static NVNdevice g_dev __attribute__((aligned(64))); static NVNdeviceBuilder g_dev_b __attribute__((aligned(64)));
static NVNmemoryPool g_pool __attribute__((aligned(64))); static NVNmemoryPoolBuilder g_pool_b __attribute__((aligned(64)));
static NVNqueue g_queue __attribute__((aligned(64))); static NVNqueueBuilder g_queue_b __attribute__((aligned(64)));
static NVNcommandBuffer g_cb __attribute__((aligned(64)));
static NVNtexture g_rt __attribute__((aligned(64))), g_depth __attribute__((aligned(64)));
static NVNtextureBuilder g_tb __attribute__((aligned(64)));
static NVNprogram g_prog __attribute__((aligned(64))), g_prog_x __attribute__((aligned(64)));
static NVNblendState g_blend __attribute__((aligned(16))); static NVNcolorState g_color __attribute__((aligned(16)));
static NVNchannelMaskState g_mask __attribute__((aligned(16))); static NVNmultisampleState g_ms __attribute__((aligned(16)));
static NVNpolygonState g_poly __attribute__((aligned(16))); static NVNdepthStencilState g_ds __attribute__((aligned(16)));
static NVNvertexAttribState g_attr[2] __attribute__((aligned(16))); static NVNvertexStreamState g_stream __attribute__((aligned(16)));
static char g_queuemem[0x80000] __attribute__((aligned(0x1000))), g_ctl[0x1000] __attribute__((aligned(16)));
static void exit_thread(void) { register u64 x0 __asm__("x0"); __asm__ volatile("svc #0x0A" : "=r"(x0) : : "memory", "x1", "x2", "x3"); for (;;) {} }

#define POOL(off) ((NVNbufferAddress)(g_poolmem + (off)))
#define VTX 0x10000
#define UBO_VS0 0x20000      /* {0, 0, 0, 0}: no shift, depth 0 */
#define UBO_VS_NEAR 0x20100  /* depth -0.5 in NDC: window depth 0.25 */
#define UBO_VS_FAR 0x20200   /* depth +0.5: window depth 0.75 */
#define UBO_VS_SHIFT 0x20300 /* shifted right by 0.5 */
#define UBO_FS_WHITE 0x20400
#define UBO_FS_HALF 0x20500
#define IDX 0x20600       /* 16-bit indices */
#define OUT_COLOR 0xA0000
#define OUT_DEPTH 0xA8000
enum { LEFT_RED = 0, RIGHT_GREEN_CW = 3, LEFT_GREEN_HALF = 6, LEFT_WHITE = 9, LEFT_GREEN = 12 };   /* first vertex of each triangle */

static void tri(V *v, float x0, float y0, float x1, float y1, float x2, float y2, float r, float g, float b, float a) {
    v[0] = (V){x0, y0, r, g, b, a}; v[1] = (V){x1, y1, r, g, b, a}; v[2] = (V){x2, y2, r, g, b, a};
}
static void ubo4(int off, float a, float b, float c, float d) { volatile float *p = (volatile float *)(g_poolmem + off); p[0] = a; p[1] = b; p[2] = c; p[3] = d; }

static void begin(void) { nvnCommandBufferBeginRecording(&g_cb); }
static void submit(void) { NVNcommandHandle h = nvnCommandBufferEndRecording(&g_cb); nvnQueueSubmitCommands(&g_queue, 1, &h); nvnQueueFinish(&g_queue); }

// Targets, viewport, scissor and the clear every stage starts with
static void target(const float clear[4], int depth) {
    const NVNtexture *rts[1] = {&g_rt};
    nvnCommandBufferSetRenderTargets(&g_cb, 1, rts, 0, depth ? &g_depth : 0, 0);
    nvnCommandBufferSetViewport(&g_cb, 0, 0, 64, 64); nvnCommandBufferSetScissor(&g_cb, 0, 0, 64, 64);
    nvnCommandBufferClearColor(&g_cb, 0, clear, 0xF);
    if (depth) nvnCommandBufferClearDepthStencil(&g_cb, 1.0f, 1, 0, 0);
}
static void bind_all(int vs_ubo, int fs_ubo) {
    nvnCommandBufferBindProgram(&g_cb, &g_prog, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
    nvnCommandBufferBindBlendState(&g_cb, &g_blend); nvnCommandBufferBindColorState(&g_cb, &g_color); nvnCommandBufferBindChannelMaskState(&g_cb, &g_mask);
    nvnCommandBufferBindMultisampleState(&g_cb, &g_ms); nvnCommandBufferBindPolygonState(&g_cb, &g_poly); nvnCommandBufferBindDepthStencilState(&g_cb, &g_ds);
    nvnCommandBufferBindVertexAttribState(&g_cb, 2, g_attr); nvnCommandBufferBindVertexStreamState(&g_cb, 1, &g_stream);
    nvnCommandBufferBindVertexBuffer(&g_cb, 0, POOL(VTX), 15 * sizeof(V));
    nvnCommandBufferBindUniformBuffer(&g_cb, NVN_SHADER_STAGE_VERTEX, 0, POOL(vs_ubo), 16);
    nvnCommandBufferBindUniformBuffer(&g_cb, NVN_SHADER_STAGE_FRAGMENT, 0, POOL(fs_ubo), 16);
}
static void copy_out(int with_depth) {
    NVNcopyRegion whole = {0, 0, 0, 64, 64, 1};
    nvnCommandBufferCopyTextureToBuffer(&g_cb, &g_rt, 0, &whole, POOL(OUT_COLOR), 0);
    if (with_depth) nvnCommandBufferCopyTextureToBuffer(&g_cb, &g_depth, 0, &whole, POOL(OUT_DEPTH), 0);
}
static const volatile unsigned char *px(int x, int y) { return (const volatile unsigned char *)(g_poolmem + OUT_COLOR) + (y * 64 + x) * 4; }
static int is_a(int x, int y, int r, int g, int b, int tol, int a) {
    const volatile unsigned char *p = px(x, y);
    int d0 = (int)p[0] - r, d1 = (int)p[1] - g, d2 = (int)p[2] - b, d3 = (int)p[3] - a;
    return d0 >= -tol && d0 <= tol && d1 >= -tol && d1 <= tol && d2 >= -tol && d2 <= tol && d3 >= -tol && d3 <= tol;
}
#define is(x, y, r, g, b, tol) is_a((x), (y), (r), (g), (b), (tol), 255)
// the left triangle's centre in pixels: NDC (-0.45, -0.27) with row 0 at NDC y = -1
#define LX 17
#define LY 23

void guest_main(u64 arg) {
    FUNCS(LOAD)
    V *v = (V *)(g_poolmem + VTX);
    tri(v + LEFT_RED, -0.8f, -0.8f, -0.1f, -0.8f, -0.45f, 0.8f, 1, 0, 0, 1);            // counter-clockwise with y up
    tri(v + RIGHT_GREEN_CW, 0.2f, 0.1f, 0.45f, 0.8f, 0.8f, 0.1f, 0, 1, 0, 1);            // clockwise
    tri(v + LEFT_GREEN_HALF, -0.8f, -0.8f, -0.1f, -0.8f, -0.45f, 0.8f, 0, 1, 0, 0.5f);
    tri(v + LEFT_WHITE, -0.8f, -0.8f, -0.1f, -0.8f, -0.45f, 0.8f, 1, 1, 1, 1);
    tri(v + LEFT_GREEN, -0.8f, -0.8f, -0.1f, -0.8f, -0.45f, 0.8f, 0, 1, 0, 1);
    ubo4(UBO_VS0, 0, 0, 0, 0); ubo4(UBO_VS_NEAR, 0, 0, -0.5f, 0); ubo4(UBO_VS_FAR, 0, 0, 0.5f, 0); ubo4(UBO_VS_SHIFT, 0.5f, 0, 0, 0);
    ubo4(UBO_FS_WHITE, 1, 1, 1, 1); ubo4(UBO_FS_HALF, 0.5f, 0.5f, 0.5f, 1);

    // ---- device, pool, queue, command buffer, two render targets, the program, the default state
    nvnDeviceBuilderSetDefaults(&g_dev_b); nvnDeviceBuilderSetFlags(&g_dev_b, 0);
    int ok = nvnDeviceInitialize(&g_dev, &g_dev_b) & 0xFF;
    nvnMemoryPoolBuilderSetDevice(&g_pool_b, &g_dev); nvnMemoryPoolBuilderSetDefaults(&g_pool_b);
    nvnMemoryPoolBuilderSetStorage(&g_pool_b, g_poolmem, sizeof g_poolmem);
    nvnMemoryPoolBuilderSetFlags(&g_pool_b, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    ok &= nvnMemoryPoolInitialize(&g_pool, &g_pool_b) & 0xFF;
    nvnQueueBuilderSetDevice(&g_queue_b, &g_dev); nvnQueueBuilderSetDefaults(&g_queue_b);
    nvnQueueBuilderSetQueueMemory(&g_queue_b, g_queuemem, sizeof g_queuemem); nvnQueueBuilderSetCommandMemorySize(&g_queue_b, 0x40000);
    ok &= nvnQueueInitialize(&g_queue, &g_queue_b) & 0xFF;
    ok &= nvnCommandBufferInitialize(&g_cb, &g_dev) & 0xFF;
    nvnCommandBufferAddCommandMemory(&g_cb, &g_pool, 0x30000, 0x10000); nvnCommandBufferAddControlMemory(&g_cb, g_ctl, sizeof g_ctl);
    nvnTextureBuilderSetDevice(&g_tb, &g_dev); nvnTextureBuilderSetDefaults(&g_tb); nvnTextureBuilderSetTarget(&g_tb, NVN_TEXTURE_TARGET_2D); nvnTextureBuilderSetSize2D(&g_tb, 64, 64);
    nvnTextureBuilderSetFormat(&g_tb, NVN_FORMAT_RGBA8); nvnTextureBuilderSetLevels(&g_tb, 1); nvnTextureBuilderSetStorage(&g_tb, &g_pool, 0x80000);
    ok &= nvnTextureInitialize(&g_rt, &g_tb) & 0xFF;
    nvnTextureBuilderSetFormat(&g_tb, NVN_FORMAT_DEPTH32F); nvnTextureBuilderSetStorage(&g_tb, &g_pool, 0x90000);
    ok &= nvnTextureInitialize(&g_depth, &g_tb) & 0xFF;
    NVNshaderData sd[2] = {{POOL(0x0), g_vs_control}, {POOL(0x100), g_fs_control}};
    ok &= nvnProgramInitialize(&g_prog, &g_dev) & 0xFF;
    ok &= nvnProgramSetShaders(&g_prog, 2, sd) & 0xFF;
    nvnBlendStateSetDefaults(&g_blend); nvnBlendStateSetBlendTarget(&g_blend, 0); nvnColorStateSetDefaults(&g_color); nvnChannelMaskStateSetDefaults(&g_mask);
    nvnMultisampleStateSetDefaults(&g_ms); nvnPolygonStateSetDefaults(&g_poly); nvnDepthStencilStateSetDefaults(&g_ds);
    nvnDepthStencilStateSetDepthTestEnable(&g_ds, 0); nvnDepthStencilStateSetDepthWriteEnable(&g_ds, 0);
    nvnVertexAttribStateSetDefaults(&g_attr[0]); nvnVertexAttribStateSetFormat(&g_attr[0], NVN_FORMAT_RG32F, 0); nvnVertexAttribStateSetStreamIndex(&g_attr[0], 0);
    nvnVertexAttribStateSetDefaults(&g_attr[1]); nvnVertexAttribStateSetFormat(&g_attr[1], NVN_FORMAT_RGBA32F, 8); nvnVertexAttribStateSetStreamIndex(&g_attr[1], 0);
    nvnVertexStreamStateSetDefaults(&g_stream); nvnVertexStreamStateSetStride(&g_stream, sizeof(V));
    g_res[0] = ok;

    const float blue[4] = {0, 0, 1, 1}, black[4] = {0, 0, 0, 1};

    // ---- A: a counter-clockwise triangle in the default state (no culling, nothing blended or tested)
    begin(); target(blue, 0); bind_all(UBO_VS0, UBO_FS_WHITE); nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_RED, 3); copy_out(0); submit();
    g_res[1] = is(LX, LY, 255, 0, 0, 1) && is(50, 32, 0, 0, 255, 1) && is(2, 2, 0, 0, 255, 1);

    // ---- B: culling. Back faces culled, front = CCW: the CCW triangle is drawn and the CW one is not
    nvnPolygonStateSetCullFace(&g_poly, NVN_FACE_BACK); nvnPolygonStateSetFrontFace(&g_poly, NVN_FRONT_FACE_CCW);
    begin(); target(blue, 0); bind_all(UBO_VS0, UBO_FS_WHITE);
    nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_RED, 3); nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, RIGHT_GREEN_CW, 3);
    copy_out(0); submit();
    g_res[2] = is(LX, LY, 255, 0, 0, 1) && is(47, 42, 0, 0, 255, 1);                                  // the clockwise one is culled
    nvnPolygonStateSetCullFace(&g_poly, NVN_FACE_FRONT);                                               // now the front faces go: the clockwise one shows and the other does not
    begin(); target(blue, 0); bind_all(UBO_VS0, UBO_FS_WHITE);
    nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_RED, 3); nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, RIGHT_GREEN_CW, 3);
    copy_out(0); submit();
    g_res[3] = is(47, 42, 0, 255, 0, 1) && is(LX, LY, 0, 0, 255, 1);
    nvnPolygonStateSetCullFace(&g_poly, NVN_FACE_NONE);

    // ---- C: blending. An opaque red triangle, then the same one in half-transparent green with SRC_ALPHA / ONE_MINUS_SRC_ALPHA: a 50 % mix
    nvnBlendStateSetBlendFunc(&g_blend, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA);
    begin(); target(black, 0); bind_all(UBO_VS0, UBO_FS_WHITE);
    nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_RED, 3);                      // blending still off
    nvnColorStateSetBlendEnable(&g_color, 0, 1); nvnCommandBufferBindColorState(&g_cb, &g_color); nvnCommandBufferBindBlendState(&g_cb, &g_blend);
    nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_GREEN_HALF, 3);
    copy_out(0); submit();
    g_res[4] = is_a(LX, LY, 128, 128, 0, 2, 191);                                 // alpha too: 0.5 * 0.5 + 1 * (1 - 0.5) = 0.75
    g_res[20] = (u64)px(LX, LY)[0] | (u64)px(LX, LY)[1] << 8 | (u64)px(LX, LY)[2] << 16 | (u64)px(LX, LY)[3] << 24;           // the pixel as it came out, for diagnosis
    nvnColorStateSetBlendEnable(&g_color, 0, 0);

    // ---- D: depth. LESS with writes: the near triangle (window depth 0.25) first, a farther one (0.75) over it must not show; the stored depth is the proof
    nvnDepthStencilStateSetDepthTestEnable(&g_ds, 1); nvnDepthStencilStateSetDepthWriteEnable(&g_ds, 1); nvnDepthStencilStateSetDepthFunc(&g_ds, NVN_DEPTH_FUNC_LESS);
    begin(); target(black, 1); bind_all(UBO_VS_NEAR, UBO_FS_WHITE);
    nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_RED, 3);
    nvnCommandBufferBindUniformBuffer(&g_cb, NVN_SHADER_STAGE_VERTEX, 0, POOL(UBO_VS_FAR), 16);          // the next draw reads another uniform buffer
    nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_GREEN, 3);
    copy_out(1); submit();
    g_res[5] = is(LX, LY, 255, 0, 0, 1);                                                                 // the far green one lost the depth test
    g_res[6] = *(volatile float *)(g_poolmem + OUT_DEPTH + (LY * 64 + LX) * 4) == 0.25f;                 // window depth = (-0.5 + 1) / 2: the -w..w clip volume
    { union { float f; unsigned u; } d0, d1; d0.f = *(volatile float *)(g_poolmem + OUT_DEPTH + (LY * 64 + LX) * 4); d1.f = *(volatile float *)(g_poolmem + OUT_DEPTH + (2 * 64 + 2) * 4); g_res[21] = d0.u; g_res[22] = d1.u; }
    g_res[7] = *(volatile float *)(g_poolmem + OUT_DEPTH + (2 * 64 + 2) * 4) == 1.0f;                    // untouched pixels keep the clear
    nvnDepthStencilStateSetDepthTestEnable(&g_ds, 0); nvnDepthStencilStateSetDepthWriteEnable(&g_ds, 0);

    // ---- E: a channel mask. Only red is written, so a white triangle on black comes out red
    nvnChannelMaskStateSetChannelMask(&g_mask, 0, 1, 0, 0, 0);
    begin(); target(black, 0); bind_all(UBO_VS0, UBO_FS_WHITE); nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_WHITE, 3); copy_out(0); submit();
    g_res[8] = is(LX, LY, 255, 0, 0, 1);
    nvnChannelMaskStateSetChannelMask(&g_mask, 0, 1, 1, 1, 1);

    // ---- F: uniform buffers. The vertex one shifts the triangle right, the fragment one halves its colour
    begin(); target(black, 0); bind_all(UBO_VS_SHIFT, UBO_FS_HALF); nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_WHITE, 3); copy_out(0); submit();
    g_res[9] = is(33, LY, 128, 128, 128, 2) && is(LX, LY, 0, 0, 0, 0);

    // ---- G: an indexed draw. The indices pick the triangle's vertices, in their order: 12, 13, 14 is the CCW green one; 14, 13, 12 is its mirror image, culled
    volatile unsigned short *ix = (volatile unsigned short *)(g_poolmem + IDX); ix[0] = 12; ix[1] = 13; ix[2] = 14; ix[3] = 14; ix[4] = 13; ix[5] = 12;
    begin(); target(blue, 0); bind_all(UBO_VS0, UBO_FS_WHITE); nvnCommandBufferDrawElements(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 3, POOL(IDX)); copy_out(0); submit();
    g_res[11] = is(LX, LY, 0, 255, 0, 1) && is(50, 32, 0, 0, 255, 1);
    nvnPolygonStateSetCullFace(&g_poly, NVN_FACE_BACK);
    begin(); target(blue, 0); bind_all(UBO_VS0, UBO_FS_WHITE); nvnCommandBufferDrawElements(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 3, POOL(IDX + 6)); copy_out(0); submit();
    g_res[12] = is(LX, LY, 0, 0, 255, 1);
    nvnPolygonStateSetCullFace(&g_poly, NVN_FACE_NONE);

    // ---- H: a program whose shaders are not in the registry. The draw is skipped, the rest of the frame is not harmed
    NVNshaderData sx[2] = {{POOL(0x200), g_x_control}, {POOL(0x300), g_x_control}};
    int okx = nvnProgramInitialize(&g_prog_x, &g_dev) & 0xFF; okx &= nvnProgramSetShaders(&g_prog_x, 2, sx) & 0xFF;
    begin(); target(blue, 0); bind_all(UBO_VS0, UBO_FS_WHITE); nvnCommandBufferBindProgram(&g_cb, &g_prog_x, NVN_SHADER_STAGE_ALL_GRAPHICS_BITS);
    nvnCommandBufferDrawArrays(&g_cb, NVN_DRAW_PRIMITIVE_TRIANGLES, LEFT_RED, 3); copy_out(0); submit();
    g_res[13] = okx && is(LX, LY, 0, 0, 255, 1);

    g_res[10] = 0x5555;
    exit_thread();
}
