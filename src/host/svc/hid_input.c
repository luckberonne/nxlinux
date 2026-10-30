// hid shared memory: player 1's npad is a Pro Controller that is always connected; its state ring is refilled at 120 Hz from the last hid_input_set.
#include "hid_input.h"
#include "../ipc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NPAD_BASE   0x9A00u     // 10 entries of 0x5000: players 1..8, handheld, other
#define LIFO_FULLKEY 0x28u      // within an entry: style, assignment, colours, then the lifos of 0x350 bytes
#define LIFO_COUNT  17u
#define STYLE_FULLKEY 1u

static ipc_shmem_t g_hid_blob = {NULL, 0x40000};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_buttons; static int32_t g_stick[4];
static uint64_t g_sample;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

ipc_shmem_t *hid_block(void) { return &g_hid_blob; }

void hid_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    pthread_mutex_lock(&g_mu); g_buttons = buttons; g_stick[0] = lx; g_stick[1] = ly; g_stick[2] = rx; g_stick[3] = ry; pthread_mutex_unlock(&g_mu);
}

static void publish(uint8_t *mem) {
    uint8_t *e = mem + NPAD_BASE;
    uint64_t buttons; int32_t st[4];
    static int tap = -2; static unsigned period = 4;                // NX_HID_TAP=<button bit>[:<period in s>]: presses that button for 150 ms every period, for runs nobody sits at
    if (tap == -2) { const char *t = getenv("NX_HID_TAP"); tap = t ? atoi(t) : -1; if (t && strchr(t, ':')) period = (unsigned)atoi(strchr(t, ':') + 1); if (!period) period = 4; }
    pthread_mutex_lock(&g_mu); buttons = g_buttons; memcpy(st, g_stick, sizeof st); pthread_mutex_unlock(&g_mu);
    static struct { uint32_t at, len; int bit; } scr[64]; static int nscr = -1; static uint64_t t_start;   // NX_HID_SCRIPT="<s>:<bit>[:<ms>],...": presses at seconds after the block is mapped
    if (nscr < 0) {
        nscr = 0; const char *p = getenv("NX_HID_SCRIPT");
        while (p && *p && nscr < 64) { double s = strtod(p, (char **)&p); if (*p != ':') break; p++; int bit = (int)strtol(p, (char **)&p, 10); uint32_t len = 200; if (*p == ':') { p++; len = (uint32_t)strtoul(p, (char **)&p, 10); }
            scr[nscr].at = (uint32_t)(s * 1000); scr[nscr].bit = bit; scr[nscr].len = len; nscr++; if (*p == ',') p++; }
    }
    if (nscr) { struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); uint64_t ms = (uint64_t)t0.tv_sec * 1000 + (uint64_t)t0.tv_nsec / 1000000; if (!t_start) t_start = ms;
        static int said[64];
        for (int i = 0; i < nscr; i++) if (ms - t_start >= scr[i].at && ms - t_start < (uint64_t)scr[i].at + scr[i].len) {
            buttons |= 1ull << scr[i].bit;
            if (!said[i]) { said[i] = 1; fprintf(stderr, "[dm] [hid] script: button bit %d at %.1f s\n", scr[i].bit, (ms - t_start) / 1000.0); } } }
    if (tap >= 0) { struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); uint64_t ms = (uint64_t)t0.tv_sec * 1000 + (uint64_t)t0.tv_nsec / 1000000; if (ms % (period * 1000ull) < 150) buttons |= 1ull << tap; }
    uint64_t n = ++g_sample;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ticks = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) * 12 / 625;   // the 19.2 MHz system counter
    uint32_t style = STYLE_FULLKEY;
    memcpy(e + 0, &style, 4);
    uint8_t *lifo = e + LIFO_FULLKEY;
    uint64_t tail, count;
    memcpy(&tail, lifo + 0x10, 8); memcpy(&count, lifo + 0x18, 8);
    tail = (tail + 1) % LIFO_COUNT; if (count < LIFO_COUNT) count++;
    uint8_t *s = lifo + 0x20 + tail * 0x30;
    uint32_t attr = 3;                                             // connected, wired
    uint8_t rec[0x30] = {0};
    memcpy(rec + 0x00, &n, 8); memcpy(rec + 0x08, &n, 8); memcpy(rec + 0x10, &buttons, 8);
    memcpy(rec + 0x18, st, 16); memcpy(rec + 0x28, &attr, 4);
    memcpy(s, rec, sizeof rec);
    uint64_t cap = LIFO_COUNT;
    memcpy(lifo + 0x00, &ticks, 8); memcpy(lifo + 0x08, &cap, 8);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(lifo + 0x10, &tail, 8); memcpy(lifo + 0x18, &count, 8);
}

static void *pump(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t a = __atomic_load_n(&g_hid_blob.mapped, __ATOMIC_ACQUIRE);
        if (a) publish((uint8_t *)(uintptr_t)a);
        struct timespec d = {0, 8 * 1000 * 1000}; nanosleep(&d, NULL);
    }
    return NULL;
}
static void start_pump(void) { pthread_t t; if (!pthread_create(&t, NULL, pump, NULL)) pthread_detach(t); }
void hid_start(void) { pthread_once(&g_once, start_pump); }
