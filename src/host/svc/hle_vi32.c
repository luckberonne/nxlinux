// nn::vi (the display layer) and nv (the GPU driver glue under NVN) of an AArch32 title, taken over under the names that process mangles: size_t is `j` and a 64-bit integer is `y`, and
// this SDK's CreateLayer takes the layer flags by value. The host has no vi or nvdrv service for them to talk to (NVN is replaced wholesale, see hle_nvn.c), so the display, layer and
// native window are static host objects and the nv calls only record what the game gives them. Same design as the AArch64 names in nxnative's hle_vi.c / hle_nv.c.
#include "hle.h"

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "display service init; the host has no vi service", void, h32_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "display service shutdown; the host has no vi service", void, h32_vi_Fini, void) {}
HLE_FORCE("_ZN2nn2vi16InitializeMemoryEv", "vi buffer memory; the host presents through its own swapchain", void, h32_vi_InitMem, void) {}
HLE_FORCE("_ZN2nn2vi14FinalizeMemoryEv", "vi buffer memory; the host presents through its own swapchain", void, h32_vi_FiniMem, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "open default display; the host returns a static object", Result, h32_vi_OpenDefault, void **out) {
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11OpenDisplayEPPNS0_7DisplayEPKc", "open a display by name; the host has one, a static object", Result, h32_vi_OpenDisp, void **out, const char *name) {
    (void)name; *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "close display; host static object", void, h32_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayENS_4util10BitFlagSetILi32ENS0_10LayerFlagsEEE", "create layer; the host returns a static object",
          Result, h32_vi_CreateLayer, void **out, void *disp, uint32_t flags) {
    (void)disp; (void)flags; *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "create layer with default settings; the host returns a static object",
          Result, h32_vi_CreateLayer2, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "destroy layer; host static object", void, h32_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "native window handle, passed to the NVN window builder; the host returns a static object", Result, h32_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "layer scaling mode; the host present does its own scaling", Result, h32_vi_Scaling, void *layer, int mode) {
    (void)layer; (void)mode; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi12SetLayerSizeEPNS0_5LayerEii", "layer size; an NVN window's textures decide what is presented", Result, h32_vi_LayerSize, void *layer, int w, int h) {
    (void)layer; LOG("vi::SetLayerSize %dx%d", w, h); return RES_OK;
}
HLE_FORCE("_ZN2nn2vi9SetLayerZEPNS0_5LayerEi", "layer z order; one layer", Result, h32_vi_LayerZ, void *layer, int z) { (void)layer; (void)z; return RES_OK; }
HLE_FORCE("_ZN2nn2vi18SetLayerVisibilityEPNS0_5LayerEb", "layer visibility; always shown", Result, h32_vi_Visible, void *layer, int visible) { (void)layer; (void)visible; return RES_OK; }

// nv: the SDK's graphics glue, which connects to nvdrv and builds transfer memory. NVN does not run on it, so nothing is done. SetGraphicsAllocator and
// SetGraphicsDevtoolsAllocator are NOT taken over: the SDK keeps the game's allocator in its own state and nn::vi (CreateLayer) allocates through it.
#define WHY "NVN has been replaced wholesale with a host device, so the driver glue layer has nothing to do"
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvj", WHY, void, h32_nv_Init, void *mem, uint32_t size) { (void)mem; (void)size; }
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY, void, h32_nv_Fin, void) {}
HLE_FORCE("_ZN2nv22SetGraphicsServiceNameEPKc", WHY, void, h32_nv_SvcName, const char *name) { (void)name; }
HLE_FORCE("_ZN2nv22DumpGraphicsMemoryInfoEv", WHY, void, h32_nv_Dump, void) {}

// The display's vertical sync event: a SystemEvent the game waits on to pace its frames. A host thread signals it at 60 Hz once the game has asked for it.
#include <pthread.h>
#include <time.h>
static void *vsync_thread(void *arg) {
    void *ev = arg;
    for (;;) {
        struct timespec ts = {0, 16666667}; nanosleep(&ts, NULL);
        hle_signal_system_event(ev);
    }
    return NULL;
}
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "the display's vsync event; the host paces it at 60 Hz", Result, h32_vi_Vsync, void *ev, void *disp) {
    (void)disp;
    hle_bind_system_event(ev, 1);
    pthread_t t; pthread_create(&t, NULL, vsync_thread, ev); pthread_detach(t);
    return RES_OK;
}
