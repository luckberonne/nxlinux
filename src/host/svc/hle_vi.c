// nn::vi for 64-bit titles built with a system 20+ SDK. The host has no display service: a title that draws through NVN gets its window from the NVN layer, and no vi layer is ever
// created, so the layer and display pointers the title keeps are null. The swap interval calls it makes on them (the Switch 2 extensions) have nothing to do here.
#include "hle.h"

HLE_FORCE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", "clear the layer's extended swap interval (Switch 2); the host presents at its own pace and creates no vi layer",
          void, h_vi_ClearExtSwap, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", "tell the display the title handles variable refresh rate (Switch 2); the host has no display service",
          void, h_vi_VrrCap, void *display, int on) { (void)display; (void)on; }
