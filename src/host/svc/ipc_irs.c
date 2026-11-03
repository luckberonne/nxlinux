// irs / irs:sys: the infrared camera of the Joy-Con (nn::irsensor). The host has none: the service answers every command with success and no data, and the shared memory it hands out
// (command 304) is a block of zeros, so a title that initialises the sensor finds it idle. Mario Party Superstars opens it at start (its IR minigames) and aborts without the service.
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>

#define OUT32(v) (out->out[out->nout++] = (uint32_t)(v))
static ipc_shmem_t g_irs_blob;

static uint32_t irs(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 304: {                                                   // GetIrsensorSharedMemoryHandle(aruid) -> the block the sensor writes its samples to
        if (!g_irs_blob.data) { g_irs_blob.size = 0x8000; g_irs_blob.data = calloc(1, g_irs_blob.size); }
        out->copy[out->ncopy++] = ipc_new_session_data("shmem", NULL, &g_irs_blob); return 0;
    }
    case 311: OUT32(0); return 0;                                 // GetNpadIrCameraHandle(npad id) -> a handle (a plain number)
    default: {
        static unsigned seen[512]; unsigned k = in->cmd & 511;
        if (seen[k] != in->cmd + 1u) { seen[k] = in->cmd + 1u; fprintf(stderr, "[dm] irs: command %u answered with success and no data\n", in->cmd); }
        return 0;
    }
    }
}
__attribute__((constructor)) static void register_irs(void) { ipc_register_service("irs", irs); ipc_register_service("irs:sys", irs); }
