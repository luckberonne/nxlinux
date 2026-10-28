// Host services hle_nvn.c expects from the rest of the host. Clocks and the frame tick are real; the rest report "not available" until
// the services they belong to exist (a window, a crash reporter).
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
    if (__start_swhle) for (const hle_ent *e = __start_swhle; e < __stop_swhle; e++) hle_register(e->name, e->fn);
    if (__start_swforce) for (const hle_force_ent *e = __start_swforce; e < __stop_swforce; e++) hle_register(e->name, e->fn);
}
