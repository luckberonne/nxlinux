// Horizon IPC at the SVC level: ConnectToNamedPort, SendSyncRequest and CloseHandle on the sessions the host serves. A title that links the SDK's service clients (every retail title)
// talks HIPC through the message buffer at the start of its thread-local region; the host answers in place, the way a service process would. The message format is the same for
// 32- and 64-bit processes (32-bit words), so nothing here depends on the CPU mode. sm: is built in; ipc_register_service adds the others.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

#define IPC_MAX_ARGS 32
typedef struct { uint64_t addr; uint32_t size; } ipc_buf_t;   // a buffer the client passed: guest address == host address
typedef struct {
    uint32_t cmd;                       // CMIF command id (or the control command for a control message)
    int control;                        // 1: a control message (type 5/7), not a service request
    uint32_t nargs; uint32_t args[IPC_MAX_ARGS];   // the data words after the 16-byte CMIF header
    int send_pid; uint32_t ncopy, copy[8], nmove, move[8];   // handles that came with the request
    void *data;                         // the state the object being called was created with (ipc_new_session_data), NULL if none
    unsigned nsend, nrecv; ipc_buf_t send[4], recv[4];       // input (A, X) and output (B) buffers, in the order of the descriptors
} ipc_in_t;
typedef struct {
    uint32_t result;                    // Result code of the reply, 0 = success
    uint32_t nout; uint32_t out[IPC_MAX_ARGS];     // data words after the SFCO header
    uint32_t ncopy, copy[4], nmove, move[4];       // handles in the reply
} ipc_out_t;
// Handles one request on session `session` (an id the handler may keep state under). Returns 0 if handled; a nonzero value is returned to the client as the Result.
typedef uint32_t (*ipc_handler_t)(int session, const ipc_in_t *in, ipc_out_t *out);

void ipc_register_service(const char *name, ipc_handler_t fn);   // `name` as the client asks sm for it (up to 8 characters)
// Returns 1 if swi was an IPC SVC (or a CloseHandle of a session) and has been handled. x0..x7 are the AArch64-layout arguments, results go back into x0..x3.
int ipc_svc(cpu_t *c, uint32_t swi);
// A shared memory block the host hands out (a session named "shmem" whose data is one of these): svcMapSharedMemory zeroes the guest's mapping, then copies data into it.
typedef struct { const uint8_t *data; size_t size; uint64_t mapped; } ipc_shmem_t;   // `mapped`: where the guest last mapped it (0 = not mapped), for a block the host keeps writing into
void fsp_set_romfs(const char *path);   // the RomFS image fsp-srv serves as the title's data storage
uint32_t ipc_new_session_data(const char *name, ipc_handler_t fn, void *data);   // as ipc_new_session, with state the handler finds in in->data (it stays with the object when the session becomes a domain object)
uint32_t ipc_new_session(const char *name, ipc_handler_t fn);   // a session handle for a service the host answers, for a handler that hands out objects
// As ipc_new_session_data, for state that must be freed: fin(data) runs when the last session or domain object holding it closes (clones share it)
uint32_t ipc_new_session_owned(const char *name, ipc_handler_t fn, void *data, void (*fin)(void *));
