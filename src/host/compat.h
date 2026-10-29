// The few Apple-only calls nxnative's host code uses, for the Linux host. Included by hle.h.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t clock_gettime_nsec_np(clockid_t id) { struct timespec ts; clock_gettime(id, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static inline uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }       // already nanoseconds: the timebase below is 1/1
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; return 0; }
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
