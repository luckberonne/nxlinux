// Horizon thread and synchronization SVCs, implemented on host threads. Each guest thread is a pthread running its own cpu_t.
// Semantics follow the public Horizon kernel (Atmosphère mesosphère: KConditionVariable, KAddressArbiter, KSynchronization).
// All waits hang on one global lock with one condition variable per thread: correctness first, performance later.
#pragma once
#include <stdint.h>
#include "cpu.h"

#define R_TIMEOUT    0xEA01u   // Kernel module 1, description 117
#define R_CANCELLED  0xEC01u   // 118
#define R_BAD_HANDLE 0xE401u   // 114
#define R_BAD_STATE  0xFA01u   // 125
#define R_INVALID_ADDRESS 0xCC01u

void kernel_init(void);
// Handles one SVC for the calling guest thread (x0..x7 in, x0..x3 out). Returns 0 if swi is not a thread/sync SVC.
int kernel_svc(cpu_t *c, uint32_t swi);
// Runs entry(arg) as the main guest thread on the calling host thread; returns when it exits.
// Returns the exit status: 0 when the thread called ExitThread.
int kernel_run_main(uint64_t entry, uint64_t arg, uint64_t sp);
cpu_t *kernel_cpu(void);                   // the calling guest thread's cpu, NULL on a host thread (what a host function needs to call back into the guest: cpu_call32)
uint32_t kernel_event_new_pair(uint32_t *writable, uint32_t *readable);   // both ends of a kernel event, as svcCreateEvent makes them
void kernel_event_clear(uint32_t readable);
uint32_t kernel_event_new(int signaled);   // a readable event the host owns (for services that hand the guest an event): the guest waits on and closes the handle
void kernel_event_signal(uint32_t handle);
void kernel_note_ipc(const char *name, uint32_t cmd);   // the thread dump shows which request a thread is inside of: call with the service name before the handler and NULL after
void kernel_core_release(void);            // for a host function that waits outside the kernel: free the calling guest thread's core while it waits, take it again after
void kernel_core_acquire(void);
uint64_t kernel_tls(void);             // the calling guest thread's thread-local region (its IPC message buffer is at the start)
uint64_t kernel_cntpct(void);          // the 19.2 MHz system counter
int kernel_live_threads(void);         // guest threads started and not yet exited, the main thread included
void kernel_wait_all(void);            // block until every guest thread but the caller's has exited
