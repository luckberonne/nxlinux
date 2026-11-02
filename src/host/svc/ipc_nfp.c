// nfp:user, the amiibo service (nn::nfp). The host has no NFC reader: the interface exists, is initialised, lists no device and never reports a tag. Super Smash Bros. Ultimate
// opens it when its main menu comes up (amiibo are part of its menus) and aborts without the service. Command ids: switchbrew "NFC services" (nfp:user, IUser).
#include "ipc.h"
#include "kernel.h"
#include <stdio.h>

#define OUT32(v) (out->out[out->nout++] = (uint32_t)(v))

static uint32_t nfp_user(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: case 1: return 0;                                     // Initialize, Finalize
    case 2: OUT32(0); return 0;                                   // ListDevices -> the number of devices: none
    case 17: case 18: case 23: out->copy[out->ncopy++] = kernel_event_new(0); return 0;     // AttachActivateEvent, AttachDeactivateEvent, AttachAvailabilityChangeEvent: events that never fire
    case 19: OUT32(1); return 0;                                  // GetState -> 1: initialised
    case 20: OUT32(5); return 0;                                  // GetDeviceState -> 5: unavailable
    case 21: OUT32(0); return 0;                                  // GetNpadId -> player 1
    default: {
        static unsigned seen[64]; unsigned k = in->cmd & 63;
        if (seen[k] != in->cmd + 1u) { seen[k] = in->cmd + 1u; fprintf(stderr, "[RED] ipc: nfp:user command %u is not implemented, answered with success and no data\n", in->cmd); }
        return 0;
    }
    }
}

static uint32_t nfp_manager(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    if (in->cmd == 0) { out->move[out->nmove++] = ipc_new_session("nfp.user", nfp_user); return 0; }     // CreateUserInterface -> IUser
    fprintf(stderr, "[RED] ipc: nfp:user manager command %u is not implemented\n", in->cmd);
    return 0xF201u;
}

__attribute__((constructor)) static void register_nfp(void) { ipc_register_service("nfp:user", nfp_manager); }
