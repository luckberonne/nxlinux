// nn::hid library calls the host takes over. On the console ShowControllerSupport starts the controller applet (a system screen where the players press L+R);
// nxlinux has no library applets (ILibraryAppletCreator is not implemented, and the SDK then calls through a NULL applet handle: MK8D crashed choosing Multiplayer).
// The host answers right away, as nxnative does: the players that are connected now, raised to the minimum the title asks for. Players without a pad yet are
// reserved in hid_input.c: they read as connected Pro Controllers with nothing pressed, and the next gamepad used joins as the lowest such player.
// The title then gets the Resume notification the console sends when an applet returns (hle_oe.c): MK8D stops its scene before it starts the applet and waits for it.
#include "hle.h"
#include "hid_input.h"
void dm_applet_returned(void);   // svc/hle_oe.c
#include <stdio.h>

// ControllerSupportArg starts { s8 playerCountMin, s8 playerCountMax, bool enableTakeOverConnection, bool enableLeftJustify, bool enablePermitJoyDual, bool enableSingleMode, ... }
static int controller_applet(const int8_t *arg) {
    int lo = arg ? arg[0] : 1, hi = arg ? arg[1] : 8, single = arg ? arg[5] : 0;
    if (hi < 1 || hi > 8) hi = 8;
    if (single) hi = 1;
    if (lo < 1) lo = 1;
    if (lo > hi) lo = hi;
    int have = hid_connected_count(), n = have < lo ? lo : have > hi ? hi : have;
    hid_reserve_players(n);
    fprintf(stderr, "[dm] [hid] controller applet: min %d max %d%s -> %d players (%d connected); flags %02x %02x %02x %02x %02x\n", arg ? arg[0] : 0, arg ? arg[1] : 0, single ? " single" : "", n, have,
            arg ? (uint8_t)arg[2] : 0, arg ? (uint8_t)arg[3] : 0, arg ? (uint8_t)arg[4] : 0, arg ? (uint8_t)arg[5] : 0, arg ? (uint8_t)arg[6] : 0);
    dm_applet_returned();      // the Resume notification the title gets when a library applet hands it back the screen
    return n;
}
// ControllerSupportResultInfo = { s8 playerCount, 3 padding bytes, u32 selectedId }; the selected controller is player 1's (NpadId No1 = 0)
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "controller applet; the host reports the players right away",
          Result, h_hid_ShowCtrl, int32_t *info, const int8_t *arg) {
    int n = controller_applet(arg);
    if (info) { info[0] = n; info[1] = 0; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", "controller applet (no result wanted); the host reports the players right away",
          Result, h_hid_ShowCtrl0, const int8_t *arg) { controller_applet(arg); return RES_OK; }
