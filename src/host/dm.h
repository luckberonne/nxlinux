// Common header of the Linux host. Replaces nxnative's dm/host/dm.h: the module records and ELF types live in module.h here.
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"   // constants of the current game profile (generated from games/<id>/game.tsv by games/game.py, chosen by the build's GAME)

// Explicit host work does not occupy a guest core (GPU submission, or a blocked native codec lock). A contended host mutex must count as blocked: a signal's same-core handoff
// can otherwise wait forever for a peer that needs the signaller's native lock. Uncontended locks retain guest ordering; the counter nests inside existing host work.
extern __thread _Atomic int dm_host_work;
static inline int dm_host_mutex_lock(pthread_mutex_t *mutex) {
    int r = pthread_mutex_trylock(mutex);
    if (r != EBUSY) return r;
    dm_host_work++;
    r = pthread_mutex_lock(mutex);
    dm_host_work--;
    return r;
}
// Joining a native helper releases the guest core while that helper finishes; preserve any enclosing host-work scope.
static inline int dm_host_thread_join(pthread_t thread, void **result) {
    dm_host_work++;
    int r = pthread_join(thread, result);
    dm_host_work--;
    return r;
}

#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

// SDK major version from the sdk module's version string; 0 = unknown. SDK 10 and older: NVN API 53, the shared-font address points at the TTF itself
int dm_sdk_major(void);
#define DM_SDK_OLD(major) ((major) > 0 && (major) <= 10)

// Diagnostic switches: read once from environment variables when the host starts
extern uint64_t g_t0_ns;   // Host start time (now_ns), used for relative times in the log
extern struct dm_flags { int thr, svc, fs, dbg, imports, bootprof; } g_flag;

// Page fault hook / hook before the host writes guest memory (write-protect dirty page tracking, nvn_dirty.c)
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_guest_write_prepare(void *addr, size_t len);
// Resource count callback (registered by the graphics layer): called every 250 ticks, when the loading screen has been dismissed, and on exit
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why));
void (*dm_res_report_cb(void))(uint32_t tick, const char *why);
// Graphics lifecycle (hle_nvn.c): on=1 suspends (afterwards submit / present block, and in-flight GPU work is drained before returning), on=0 resumes
void dm_gfx_suspend(int on);
void dm_gfx_enter(void); void dm_gfx_leave(void);
void dm_gfx_shot_at(long k, const char *path);
void dm_shot_written(const char *path, int ok);
void dm_guest_frame_tick(void);        // Called on every present: when DM_DETCLOCK is on, advances the guest clock by one fixed step
uint64_t dm_nvn_frame_count(void);
uint64_t dm_nvn_sub_cpu_ns(void);     // Cumulative CPU nanoseconds of submit (excluding present)
void dm_backtrace(uint64_t fp);
void crash_watch_progress(uint32_t tick, const char *stage);
void crash_watch_frame(void);
void crash_watch_pregame_done(void);
uint64_t crash_watch_progress_count(void);
void crash_dump_now(const char *why);
