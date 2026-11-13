// nn::gs, the SDK's graphics system (system 20+ SDKs): on a device it boots the graphics supervisor process (nn::gsv) through am and
// hands it a memory region that every GPU allocation then goes through. The host's graphics stack is NVN on Vulkan (src/gfx/nvn): there
// is no supervisor process, and am is never initialized (oe::Initialize is a takeover), so the SDK's boot path dereferences a null
// am::GetSelfController (ldr x8, [x0] at sdk+0x1dd77c) as soon as a title boots it.
// Takeover: the boot reports success and the region stays the game's. A title that draws through NVN imports none of the gs API
// (Ocarina of Time, the first title here built with such an SDK, imports nothing from nn::gs).
#include "hle.h"

int dm_switch2_gfx;   // set here: the title's SDK boots a graphics supervisor, so it is a Switch 2 title (see dm.h)

HLE_FORCE("_ZN2nn2gs3sdk18BootGraphicsSystemEPvm",
          "boot the graphics supervisor with a memory region; the host's graphics stack is NVN on Vulkan and has no supervisor process",
          void, h_gs_Boot, void *mem, size_t size) {
    static int once; if (!once++) LOG("gs::sdk::BootGraphicsSystem %p+%#zx: no graphics supervisor on the host (NVN on Vulkan)", mem, size);
    dm_switch2_gfx = 1;
}
