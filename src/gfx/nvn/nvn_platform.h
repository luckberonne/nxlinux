// What hle_nvn.c took from macOS, in one place. Real replacements where the value matters (clocks, resident memory, safe reads,
// SHA-256 in hle_nvn.c); diagnostic and power-management calls report failure, and the code that called them already handles that.
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <pthread.h>
#include "../../host/module.h"

#define DM_MEM_CAP_DEFAULT_GB 0

// ---- Clocks. Mach time units are nanoseconds here (numer == denom == 1)
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; return 0; }
static inline uint64_t clock_gettime_nsec_np(clockid_t id) { struct timespec ts; clock_gettime(id, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static inline uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static inline int mach_wait_until(uint64_t abs_ns) {
    struct timespec ts = {(time_t)(abs_ns / 1000000000ull), (long)(abs_ns % 1000000000ull)};
    return clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

// ---- Mach tasks and threads: statistics and scheduling hints only. Everything fails except the task's memory footprint.
typedef int kern_return_t;
typedef unsigned mach_port_t;
typedef unsigned mach_msg_type_number_t;
typedef int boolean_t;
typedef void *task_info_t, *thread_info_t, *thread_policy_t;
enum { KERN_SUCCESS = 0, KERN_FAILURE = 5 };
enum { TASK_VM_INFO = 22, TASK_POWER_INFO_V2 = 26, THREAD_BASIC_INFO = 3, THREAD_EXTENDED_INFO = 5, THREAD_TIME_CONSTRAINT_POLICY = 2 };
typedef struct { uint64_t phys_footprint; } task_vm_info_data_t;
typedef struct { struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups; } cpu_energy;
                 struct { uint64_t task_gpu_utilisation; } gpu_energy; uint64_t task_ptime, task_energy; } task_power_info_v2_data_t;
typedef struct { struct { int seconds, microseconds; } user_time, system_time; } thread_basic_info_data_t;
typedef struct { int pth_curpri, pth_priority, pth_policy; } thread_extended_info_data_t;
typedef struct { uint32_t period, computation, constraint; int preemptible; } thread_time_constraint_policy_data_t;
#define TASK_VM_INFO_COUNT 1
#define TASK_POWER_INFO_V2_COUNT 1
#define THREAD_BASIC_INFO_COUNT 1
#define THREAD_EXTENDED_INFO_COUNT 1
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT 1
static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t mach_thread_self(void) { return 0; }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
static inline kern_return_t mach_port_deallocate(mach_port_t a, mach_port_t b) { (void)a; (void)b; return KERN_SUCCESS; }
static inline kern_return_t thread_info(mach_port_t t, int f, thread_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_set(mach_port_t t, int f, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)f; (void)p; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_get(mach_port_t t, int f, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *d) { (void)t; (void)f; (void)p; (void)n; (void)d; return KERN_FAILURE; }
// The task's resident memory, from /proc: this is what the memory watchdog and the memory readout use
static inline uint64_t nx_resident_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r"); unsigned long total = 0, res = 0;
    if (f) { if (fscanf(f, "%lu %lu", &total, &res) != 2) res = 0; fclose(f); }
    return (uint64_t)res * (uint64_t)sysconf(_SC_PAGESIZE);
}
static inline kern_return_t task_info(mach_port_t t, int flavor, task_info_t out, mach_msg_type_number_t *n) {
    (void)t; (void)n;
    if (flavor == TASK_VM_INFO) { ((task_vm_info_data_t *)out)->phys_footprint = nx_resident_bytes(); return KERN_SUCCESS; }
    return KERN_FAILURE;
}
// Quality-of-service classes have no counterpart
typedef int qos_class_t;
#define QOS_CLASS_UNSPECIFIED 0
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *p) { (void)t; if (q) *q = 0; if (p) *p = 0; return 0; }
static inline int thread_selfcounts(int type, void *buf, size_t n) { (void)type; memset(buf, 0, n); return -1; }   // instruction / cycle counters
static inline int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) { (void)name; (void)oldp; (void)oldlenp; (void)newp; (void)newlen; return -1; }

// ---- Reading a pointer the guest handed over, which may not be memory at all
typedef uintptr_t vm_address_t;
typedef size_t vm_size_t;
static inline kern_return_t vm_read_overwrite(mach_port_t t, vm_address_t src, vm_size_t n, vm_address_t dst, vm_size_t *got) {
    (void)t;
    struct iovec l = {(void *)dst, n}, r = {(void *)src, n};
    ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (got) *got = k < 0 ? 0 : (vm_size_t)k;
    return k == (ssize_t)n ? KERN_SUCCESS : KERN_FAILURE;
}
