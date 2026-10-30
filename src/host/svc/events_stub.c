// Service-layer SystemEvents: nxnative binds a guest SystemEventType to a kernel event by running the SDK's own nn::os::CreateSystemEvent on a guest thread, which needs the host
// to call guest code. The Linux host cannot do that yet, so a service that signals or clears an event reaches these, which keep nothing and say so once. A game that waits
// on such an event (account, friends, oe notifications) will not be woken; the first title to need one is the cue to build the guest call.
#include "hle.h"
#include "kernel.h"
#include "module32.h"
#include <pthread.h>
#include <stdatomic.h>

static void note(const char *what) {
    static _Atomic int once;
    if (!atomic_exchange(&once, 1)) LOG("[RED] svc: %s needs a guest call into the SDK, which the host cannot make yet; service events are not delivered", what);
}
// An AArch32 title: the SystemEventType the game gave us is attached to a kernel event the host owns, by calling the SDK's own nn::os::AttachSystemEvent on the guest thread
// (cpu_call32). The host then signals the event through the kernel, from any thread.
#define MAX_EV 64
static struct { void *ev; uint32_t w, r; } g_ev[MAX_EV];
static int g_nev;
static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ev_readable(void *ev) {
    uint32_t r = 0;
    pthread_mutex_lock(&g_ev_mu);
    for (int i = 0; i < g_nev; i++) if (g_ev[i].ev == ev) r = g_ev[i].r;
    pthread_mutex_unlock(&g_ev_mu);
    return r;
}

void hle_bind_system_event(void *ev, int autoclear) {
    if (!cpu_is_aarch32()) {            // AArch64: the same through cpu_call64 and the symbol in whichever module defines it
        cpu_t *c = kernel_cpu(); extern uint64_t module_find(const char *);
        uint64_t attach = module_find("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
        if (!c || !attach) { note("binding a SystemEvent (no guest thread or no nn::os::AttachSystemEvent)"); return; }
        uint32_t w, r; kernel_event_new_pair(&w, &r);
        uint64_t args[6] = {(uint64_t)(uintptr_t)ev, r, 1, w, 1, autoclear ? 1u : 0u};
        cpu_call64(c, attach, args, 6);
        pthread_mutex_lock(&g_ev_mu); if (g_nev < MAX_EV) { g_ev[g_nev].ev = ev; g_ev[g_nev].w = w; g_ev[g_nev++].r = r; } pthread_mutex_unlock(&g_ev_mu);
        return;
    }
    cpu_t *c = kernel_cpu();
    uint32_t attach = module32_find("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    if (!c || !attach) { note("binding a SystemEvent (no guest thread or no nn::os::AttachSystemEvent)"); return; }
    uint32_t w, r; kernel_event_new_pair(&w, &r);
    // AttachSystemEvent(SystemEventType *, readable handle, readable managed, writable handle, writable managed, EventClearMode): 6 words, the last two on the stack
    uint32_t args[6] = {(uint32_t)(uintptr_t)ev, r, 1, w, 1, autoclear ? 1u : 0u};
    cpu_call32(c, attach, args, 6);
    pthread_mutex_lock(&g_ev_mu); if (g_nev < MAX_EV) { g_ev[g_nev].ev = ev; g_ev[g_nev].w = w; g_ev[g_nev++].r = r; } pthread_mutex_unlock(&g_ev_mu);
}
void hle_signal_system_event(void *ev) {
    uint32_t r = ev_readable(ev); if (r) kernel_event_signal(r);
}
void hle_clear_system_event(void *ev) {
    uint32_t r = ev_readable(ev); if (r) kernel_event_clear(r);
}
