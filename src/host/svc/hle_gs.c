// nn::gs, the SDK's graphics system (system 20+ SDKs): on a device it boots the graphics supervisor process (nn::gsv) through am and
// hands it a memory region that every GPU allocation then goes through. The host's graphics stack is NVN on Vulkan (src/gfx/nvn): there
// is no supervisor process, and am is never initialized (oe::Initialize is a takeover), so the SDK's boot path dereferences a null
// am::GetSelfController (ldr x8, [x0] at sdk+0x1dd77c) as soon as a title boots it.
// Takeover: the boot reports success and the region stays the game's. A title that draws through NVN imports none of the gs API
// (Ocarina of Time, the first title here built with such an SDK, imports nothing from nn::gs).
#include "hle.h"
#include "cpu.h"
#include "kernel.h"
#include "guest_mem.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>

extern uint64_t module_find(const char *);
void dm_guest_sleep_ns(int64_t ns);

int dm_switch2_gfx;   // set here: the title's SDK boots a graphics supervisor, so it is a Switch 2 title (see dm.h)

HLE_FORCE("_ZN2nn2gs3sdk18BootGraphicsSystemEPvm",
          "boot the graphics supervisor with a memory region; the host's graphics stack is NVN on Vulkan and has no supervisor process",
          void, h_gs_Boot, void *mem, size_t size) {
    static int once; if (!once++) LOG("gs::sdk::BootGraphicsSystem %p+%#zx: no graphics supervisor on the host (NVN on Vulkan)", mem, size);
    dm_switch2_gfx = 1;
}

// nn::codec::video (the hardware video decoder library of the same SDKs) opens a session to the graphics supervisor in its first Initialize, whose server context takes a lock
// that does not exist here: the call faults on a null critical section (Ocarina of Time's video thread). Takeover: Initialize and Finalize do nothing; a title that then
// decodes video asks for what is not there, which is logged where it happens.
HLE_FORCE("_ZN2nn5codec5video10InitializeEv", "video decoder library start: no graphics supervisor to open a session to on the host", void, h_codecvideo_Init, void) {
    static int once; if (!once++) LOG("codec::video::Initialize: no hardware video decoder on the host");
}
HLE_FORCE("_ZN2nn5codec5video8FinalizeEv", "video decoder library stop (pairs with Initialize)", void, h_codecvideo_Fin, void) {}

// nn::syncpt, the sync point library of the same SDKs: a sync point handle is a fence (16 bytes, here {id, 0}) that a GPU or decoder job reaches, and a sync point event is signalled when the fence it
// was requested for is reached. Their operations dispatch through a table of backend functions that BootGraphicsSystem fills in on a device; with the boot a takeover the table is empty and the first
// call (Ocarina of Time's movie code: CreateSyncPointEvent, then TryWaitSyncPoint) jumped to 0. Takeover: the API itself.
// A fence the host hands out (syncpt_fence_new: the video decoder's output buffers) is reached when its owner says so (syncpt_fence_signal); a handle of 0 is a fence that needs no waiting. A sync point
// event is a guest nn::os::Event, made at Create; a request for a fence not yet reached waits in a table and signals the event when the fence is reached, and a cancel forgets it and clears the event.
static uint64_t gcall(const char *name, uint64_t a, uint64_t b, uint64_t c, int n) {
    uint64_t fn = module_find(name); cpu_t *cpu = kernel_cpu();
    if (!fn || !cpu) return 0;
    uint64_t args[3] = {a, b, c};
    return cpu_call64(cpu, fn, args, n);
}
static uint64_t sp_event(const void *ev) { return (uint64_t)(uintptr_t)objmap_get(ev); }
#define SPLOG(...) do { static int l = -1; if (l < 0) l = getenv("NX_CODEC_LOG") != NULL; if (l) LOG("[syncpt] " __VA_ARGS__); } while (0)

#define FENCE_MASK 0xffff
static _Atomic uint8_t g_fence[FENCE_MASK + 1];
static _Atomic uint64_t g_fence_ctr;
static pthread_mutex_t g_req_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { const void *ev; uint64_t id; } g_req[64]; static int g_nreq;      // requests waiting for a fence
static int fence_reached(uint64_t id) { return id == 0 || atomic_load(&g_fence[id & FENCE_MASK]); }
uint64_t syncpt_fence_new(void) { uint64_t id = atomic_fetch_add(&g_fence_ctr, 1) + 1; atomic_store(&g_fence[id & FENCE_MASK], 0); return id; }
void syncpt_fence_signal(uint64_t id) {
    atomic_store(&g_fence[id & FENCE_MASK], 1);
    for (;;) {
        const void *ev = NULL;
        pthread_mutex_lock(&g_req_mu);
        for (int i = 0; i < g_nreq; i++) if (g_req[i].id == id) { ev = g_req[i].ev; g_req[i] = g_req[--g_nreq]; break; }
        pthread_mutex_unlock(&g_req_mu);
        if (!ev) break;
        uint64_t e = sp_event(ev); SPLOG("fence %llu reached: event %p (os event %#llx) signalled", (unsigned long long)id, ev, (unsigned long long)e);
        if (e) gcall("_ZN2nn2os11SignalEventEPNS0_9EventTypeE", e, 0, 0, 1);
    }
}
static void req_drop(const void *ev) {
    pthread_mutex_lock(&g_req_mu);
    for (int i = 0; i < g_nreq; i++) if (g_req[i].ev == ev) { g_req[i] = g_req[--g_nreq]; i--; }
    pthread_mutex_unlock(&g_req_mu);
}

HLE_FORCE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", "sync point event: a guest os event (the backend table is empty on the host)", Result, h_sp_Create, void *ev) {
    uint64_t e = guest_alloc(0x40, 8); if (!e) return RES_FAIL;
    gcall("_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE", e, 0, 0, 3);   // not signalled, ManualClear
    objmap_put(ev, (void *)(uintptr_t)e); SPLOG("create %p -> os event %#llx", ev, (unsigned long long)e);
    return RES_OK;
}
HLE_FORCE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", "sync point event teardown", Result, h_sp_Destroy, void *ev) {
    req_drop(ev);
    uint64_t e = (uint64_t)(uintptr_t)objmap_del(ev); SPLOG("destroy %p (os event %#llx)", ev, (unsigned long long)e); if (e) gcall("_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE", e, 0, 0, 1);
    return RES_OK;
}
HLE_FORCE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", "sync point request: the event is signalled when the fence is reached", Result, h_sp_Request, void *ev, const uint64_t *handle) {
    uint64_t id = handle ? handle[0] : 0, e = sp_event(ev);
    req_drop(ev);
    if (fence_reached(id)) { SPLOG("request %p fence %llu: already reached", ev, (unsigned long long)id); if (e) gcall("_ZN2nn2os11SignalEventEPNS0_9EventTypeE", e, 0, 0, 1); return RES_OK; }
    pthread_mutex_lock(&g_req_mu);
    if (g_nreq < (int)(sizeof g_req / sizeof g_req[0])) { g_req[g_nreq].ev = ev; g_req[g_nreq].id = id; g_nreq++; }
    pthread_mutex_unlock(&g_req_mu);
    // the fence may have been reached between the check and the table
    if (fence_reached(id)) syncpt_fence_signal(id);
    SPLOG("request %p fence %llu: waiting", ev, (unsigned long long)id);
    return RES_OK;
}
HLE_FORCE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", "sync point cancel: the request is forgotten and the event cleared", Result, h_sp_Cancel, void *ev) {
    uint64_t e = sp_event(ev); SPLOG("cancel %p (os event %#llx)", ev, (unsigned long long)e);
    req_drop(ev);
    if (e) gcall("_ZN2nn2os10ClearEventEPNS0_9EventTypeE", e, 0, 0, 1);
    return RES_OK;
}
HLE_FORCE("_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE", "sync point event poll", int, h_sp_TryWait, const void *ev) {
    uint64_t e = sp_event(ev); return e ? (int)(gcall("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE", e, 0, 0, 1) & 1) : 1;
}
HLE_FORCE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", "multiwait holder on a sync point event", Result, h_sp_Holder, void *holder, void *ev) {
    uint64_t e = sp_event(ev); SPLOG("holder %p on %p (os event %#llx)", holder, ev, (unsigned long long)e);
    if (e) gcall("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE", (uint64_t)(uintptr_t)holder, e, 0, 2);
    return RES_OK;
}

// The waits on a sync point handle itself (the movie library's fence callback polls the decoded frame's)
HLE_FORCE("_ZN2nn6syncpt3sdk16TryWaitSyncPointERKNS1_15SyncPointHandleE", "poll a sync point", int, h_sp_TryWaitPt, const uint64_t *handle) { return fence_reached(handle ? handle[0] : 0); }
HLE_FORCE("_ZN2nn6syncpt3sdk13WaitSyncPointERKNS1_15SyncPointHandleE", "wait for a sync point (without the guest core)", Result, h_sp_Wait, const uint64_t *handle) {
    uint64_t id = handle ? handle[0] : 0;
    while (!fence_reached(id)) { kernel_core_release(); dm_guest_sleep_ns(1000000); kernel_core_acquire(); }
    return RES_OK;
}
HLE_FORCE("_ZN2nn6syncpt3sdk18TimedWaitSyncPointERKNS1_15SyncPointHandleENS_8TimeSpanE", "wait for a sync point with a time limit (without the guest core)", int, h_sp_TimedWait, const uint64_t *handle, int64_t span) {
    uint64_t id = handle ? handle[0] : 0;
    for (int64_t waited = 0; !fence_reached(id); waited += 1000000) {
        if (waited >= span) return 0;
        kernel_core_release(); dm_guest_sleep_ns(1000000); kernel_core_acquire();
    }
    return 1;
}
