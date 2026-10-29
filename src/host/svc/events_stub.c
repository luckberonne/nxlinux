// Service-layer SystemEvents: nxnative binds a guest SystemEventType to a kernel event by running the SDK's own nn::os::CreateSystemEvent on a guest thread, which needs the host
// to call guest code. The Linux host cannot do that yet, so a service that signals or clears an event reaches these, which keep nothing and say so once. A game that waits
// on such an event (account, friends, oe notifications) will not be woken; the first title to need one is the cue to build the guest call.
#include "hle.h"
#include <stdatomic.h>

static void note(const char *what) {
    static _Atomic int once;
    if (!atomic_exchange(&once, 1)) LOG("[RED] svc: %s needs a guest call into the SDK, which the host cannot make yet; service events are not delivered", what);
}
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; note("binding a SystemEvent"); }
void hle_signal_system_event(void *ev) { (void)ev; note("signalling a SystemEvent"); }
void hle_clear_system_event(void *ev) { (void)ev; }
