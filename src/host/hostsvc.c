// Host services hle_nvn.c expects from the rest of the host. Clocks and the frame tick are real; the rest report "not available" until
// the services they belong to exist (a window, a crash reporter).
#include "kernel.h"
#include "dm.h"
#include "hle.h"
#include "front.h"
#include <time.h>

uint64_t g_t0_ns;
struct dm_flags g_flag;

uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t guest_ns(void) { return now_ns(); }          // no clock freezing or deterministic stepping yet
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
void dm_backtrace(uint64_t fp) { (void)fp; }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }     // the layer crop the game last set (hle_vi.c): none
int front_screen_max_fps(void) { return 0; }

// Every HLE / HLE_FORCE entry compiled into the program (hle.h). A forced entry also overrides a definition in a loaded module; that part is the loader's.
#include "hle_stub.h"
extern const hle_ent __start_swhle[] __attribute__((weak)), __stop_swhle[] __attribute__((weak));
extern const hle_force_ent __start_swforce[] __attribute__((weak)), __stop_swforce[] __attribute__((weak));
void hle_collect_sections(void) {
    if (__start_swhle) for (const hle_ent *e = __start_swhle; e < __stop_swhle; e++) if (e->name) hle_register(e->name, e->fn);
    if (__start_swforce) for (const hle_force_ent *e = __start_swforce; e < __stop_swforce; e++) if (e->name) hle_register_force(e->name, e->fn);
}

// ---------- Side table: guest object address -> host object (open addressing; 1 marks a deleted slot) ----------
#define OM_SIZE (1u << 18)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & (OM_SIZE - 1); }
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1))
        if (g_om[i].k == k || !g_om[i].k || g_om[i].k == (void *)1) { g_om[i].k = k; g_om[i].v = v; break; }
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; g_om[i].k = (void *)1; g_om[i].v = NULL; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}

// ---------- Host hooks the services call that have no owner yet ----------
void dm_guest_sleep_ns(int64_t ns) { if (ns > 0) { struct timespec ts = {ns / 1000000000ll, ns % 1000000000ll}; nanosleep(&ts, NULL); } }
void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }      // the guest clock is the wall clock; freezing it is for the test driver
void dm_thread_native_wait(int entering) { if (entering) kernel_core_release(); else kernel_core_acquire(); }                      // a host function here runs on the guest thread's own host thread, outside the kernel's lock
void dm_audio_suspend(int on) { (void)on; }                                       // there is no audio output yet
