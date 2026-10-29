// HLE common: register host implementations by Switch mangled name.
//
// Differences from nxnative's hle.h, which ran the game natively on an AArch64 host:
//  - Entries sit in the ELF sections `swhle` / `swforce` (found with __start_ / __stop_ symbols) instead of Mach-O __DATA sections.
//  - A guest call reaches a host function through an svc stub and hle_call() (hle_call.h), which maps the AAPCS64 argument registers
//    to a SysV call. The "Apple versus Switch ABI" differences in nxnative's notes (argument extension, variadics on the stack) vanish:
//    both sides are plain register arguments here. Variadic functions still cannot use C's "..." (hle_call carries registers only).
//  - HLE_FORCE_BIG is HLE_FORCE: guest threads run guest code in the emulator and the host function runs on the thread's own big host stack.
#pragma once
#include "dm.h"
#include "compat.h"
typedef struct { const char *name; void *fn; } hle_ent;
#define HLE(mangled, ret, cname, ...)                                                             \
    static ret cname(__VA_ARGS__);                                                                \
    __attribute__((used, section("swhle"))) static const hle_ent hle_ent_##cname = {mangled, (void *)cname}; \
    static ret cname(__VA_ARGS__)

typedef uint32_t Result;   // nn::Result: w0, 0 = success
#define RES_OK 0u
#define RES_FAIL 0x2A2u      // Generic failure (cut features such as networking uniformly return this)

// Guest address -> host object side table
void *objmap_get(const void *guest);
void objmap_put(const void *guest, void *host);
void *objmap_del(const void *guest);
uint64_t now_ns(void);
// A blocking native HLE wait releases the guest core. With the condition mutex
// held, enter with 1 before waiting and balance with 0 after reacquiring it.
void dm_thread_native_wait(int entering);
uint64_t guest_ns(void);              // Guest clock (does not advance while frozen, guest_clock_freeze)
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
uint64_t dm_guest_frame(void);        // Incremented each time the host presents a frame; unaffected by machine load
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // Freeze by reason (1 background, 2 system keyboard)
void guest_clock_read_wait(void);     // A guest read of the system counter: waits while the app is in the background

// Forced takeover: even if the SDK module has a definition, all references to that symbol at load time (JUMP_SLOT/GLOB_DAT/ABS64) are resolved to the host implementation.
// Used only for the service-layer nn:: (service clients of fs/oe/aoc/pctl...); the SDK's libc++ and nn::os internals do not go into this table.
// Every entry must state the reason for the takeover; an entry in the table that no module references makes loading report a failure and abort (a wrong list must fail loudly).
// 32 bytes: the compiler aligns a static record of 24 to 16 in its own section and leaves holes between entries, which the walk over the section would trip on
typedef struct { const char *name; void *fn; const char *why; const void *pad; } hle_force_ent;
#define HLE_FORCE(mangled, why, ret, cname, ...)                                                   \
    static ret cname(__VA_ARGS__);                                                                \
    __attribute__((used, section("swforce"))) static const hle_force_ent hle_force_##cname = {mangled, (void *)cname, why, 0}; \
    static ret cname(__VA_ARGS__)
#define HLE_FORCE_BIG HLE_FORCE

// Service-layer SystemEvents (svc/events_stub.c): bind a guest SystemEventType to a kernel event, signal it, clear it
void hle_bind_system_event(void *ev, int autoclear);
void hle_signal_system_event(void *ev);
void hle_clear_system_event(void *ev);

// Registers every HLE and HLE_FORCE entry of the program with hle_register (hle_stub.h). Call once at startup, before relocating modules.
void hle_collect_sections(void);

// Operation mode (0 handheld / 1 docked) and focus: owned by the service layer, the front end can switch them at runtime
int dm_opmode(void);
void dm_set_opmode(int m);
void dm_focus_set(int st);
extern uint8_t *dm_fps_state;   // frame step state page; NULL when the build has none
void dm_audio_suspend(int on);
uint64_t dm_audio_render_calls(void);
void dm_audio_reset(void);
