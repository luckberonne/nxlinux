#include "kernel.h"
#include "guest_mem.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u
#define MAX_H 4096
#define TLR_SIZE 0x200
#define SVC_THREAD_RETURNED 0x7FF   // the landing pad a thread entry function returns to (Horizon requires ExitThread)

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W };
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    int started, exited, cancel, closed;
    pthread_t pt; pthread_cond_t cv;
    cpu_t *cpu; uint64_t tlr;
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres; uint64_t wseq;
    const uint32_t *whs; int whn;
    struct thr *next;   // wait chain
    struct thr *all;    // all-threads chain
} thr_t;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51, g_wseq;
static thr_t *g_wait, *g_all;
static uint64_t g_landing;
static pthread_cond_t g_exit_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_tlr_free[1024]; static int g_ntlr_free;

static void die(const char *m) { fprintf(stderr, "[RED] kernel: %s\n", m); abort(); }

uint64_t kernel_cntpct(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return (ns / 625) * 12 + (ns % 625) * 12 / 625;       // ns * 19.2e6 / 1e9 = ns * 12 / 625
}


static uint32_t h_new(int type, void *obj) {                // holding G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("handle table full");
    return 0;
}
static void *h_peek(uint32_t h, int type) {
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("thread SVC called from a non-guest thread");
    return t;
}

static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// A waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G).
// kind 1 matches by lock address, 2 by condition key, 3 by address
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) {
    wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv);
}
// Suspend until woken or timed out (ns < 0 infinite, 0 immediate timeout); returns the wake result or R_TIMEOUT (holding G)
static int64_t block(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    if (ns > 0) {
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    }
    while (!t->woke) {
        int rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline uint64_t ld64(uint64_t a) { return __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// Make t take the lock at addr: free, it gets it and wakes; otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // inserted anew into the owner's queue: after the existing lock waiters at equal priority
}

static int sig_obj(uint32_t h, int *bad) {    // 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
// A synchronization object became signaled: wake the WaitSynchronization waiters whose handle set holds it, so they re-check (holding G)
static void wake_sync_waiters(const void *obj) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next;
        if (w->wkind == 4) { int hit = !w->whs;
            for (int i = 0; i < w->whn && !hit; i++) { unsigned k = w->whs[i] - 0x1000; hit = k > 0 && k < MAX_H && g_h[k].obj == obj; }
            if (hit) wake(w, -1); }
        w = n; }
}

// ---------- Threads ----------
static void on_svc(cpu_t *c, uint32_t swi, void *user);
static void on_fault(cpu_t *c, uint64_t pc, int kind, void *user);
static uint64_t cntpct_cb(void) { return kernel_cntpct(); }
extern int hle_dispatch(cpu_t *c, uint32_t swi);      // hle_stub.c

static uint64_t tlr_alloc(void) {                      // holding G
    if (g_ntlr_free) return g_tlr_free[--g_ntlr_free];
    uint64_t a = guest_alloc(TLR_SIZE, 0x200);
    if (!a) die("guest window full (thread local region)");
    return a;
}

static void thread_attach(thr_t *t) {                  // on the thread's own host thread; sets up its cpu
    pthread_setspecific(g_keythr, t);
    pthread_mutex_lock(&G);
    t->tlr = tlr_alloc();
    pthread_mutex_unlock(&G);
    memset((void *)t->tlr, 0, TLR_SIZE);
    cpu_ops_t ops = {on_svc, cntpct_cb, on_fault};
    t->cpu = cpu_create(&ops, t, 0);
    cpu_set_tls(t->cpu, 0, t->tlr);                     // tpidrro_el0 = ThreadLocalRegion; the SDK sets tpidr_el0 itself
}

static void thr_free(thr_t *t) {                        // holding G
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}

static void thread_detach_exit(thr_t *t) {              // the thread's run loop has ended
    cpu_destroy(t->cpu); t->cpu = NULL;
    pthread_mutex_lock(&G);
    if (g_ntlr_free < 1024) g_tlr_free[g_ntlr_free++] = t->tlr;
    t->exited = 1;
    wake_sync_waiters(t);                               // WaitSynchronization on this thread re-checks
    pthread_cond_broadcast(&g_exit_cv);
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
}

static void run_guest(thr_t *t) {
    cpu_set_pc(t->cpu, t->entry); cpu_set_sp(t->cpu, t->stack_top);
    cpu_set_x(t->cpu, 0, t->arg); cpu_set_x(t->cpu, 30, g_landing);
    cpu_run(t->cpu);
}

static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    thread_attach(t);
    run_guest(t);
    thread_detach_exit(t);
    return NULL;
}

void kernel_init(void) {
    pthread_key_create(&g_keythr, NULL);
    g_landing = guest_alloc(16, 16);
    *(uint32_t *)g_landing = 0xD4000001u | (SVC_THREAD_RETURNED << 5);   // svc #SVC_THREAD_RETURNED
}

static thr_t *thr_new(uint64_t entry, uint64_t arg, uint64_t sp, int prio, int core) {
    thr_t *t = calloc(1, sizeof *t);
    pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&t->cv, &ca); pthread_condattr_destroy(&ca);
    t->entry = entry; t->arg = arg; t->stack_top = sp & ~0xFull;
    t->prio = prio; t->core = core >= 0 ? core : 0; t->mask = 1ull << t->core;
    return t;
}

int kernel_run_main(uint64_t entry, uint64_t arg, uint64_t sp) {
    thr_t *t = thr_new(entry, arg, sp, 44, 0);
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->started = 1; t->pt = pthread_self(); t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    thread_attach(t);
    run_guest(t);
    thread_detach_exit(t);
    return 0;
}

int kernel_live_threads(void) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) n += t->started && !t->exited;
    pthread_mutex_unlock(&G);
    return n;
}
void kernel_wait_all(void) {
    pthread_mutex_lock(&G);
    for (;;) {
        int n = 0;
        for (thr_t *t = g_all; t; t = t->all) n += t->started && !t->exited && t != pthread_getspecific(g_keythr);
        if (!n) break;
        pthread_cond_wait(&g_exit_cv, &G);
    }
    pthread_mutex_unlock(&G);
}

// ---------- SVCs ----------
static void on_fault(cpu_t *c, uint64_t pc, int kind, void *user) {
    (void)c; thr_t *t = user;
    fprintf(stderr, "[RED] guest thread %#x faulted: kind %d at pc=%#llx\n", t->handle, kind, (unsigned long long)pc);
}

static void on_svc(cpu_t *c, uint32_t swi, void *user) {
    (void)user;
    if (kernel_svc(c, swi)) return;
    if (hle_dispatch(c, swi)) return;
    fprintf(stderr, "[RED] unimplemented svc %#x (lr=%#llx)\n", swi, (unsigned long long)cpu_get_x(c, 30));
    cpu_set_x(c, 0, 0xF201u);   // Svc::ResultNotImplemented
}

int kernel_svc(cpu_t *c, uint32_t num) {
    uint64_t regs[8];
    for (int i = 0; i < 8; i++) regs[i] = cpu_get_x(c, i);
    int handled = 1;
    switch (num) {
    case SVC_THREAD_RETURNED:
        fprintf(stderr, "[RED] guest thread entry function returned (Horizon requires ExitThread)\n");
        cpu_halt(c); break;
    case 0x08: {  // CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        // Core -2 (IdealCoreUseProcessValue) is the process's default ideal core, 0, with an affinity mask of just that core
        thr_t *t = thr_new(regs[1], regs[2], regs[3], (int)regs[4], (int)regs[5]);
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // the guest waits through the handle, no join
        pthread_t pt;
        pthread_mutex_lock(&G); t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        if (pthread_create(&pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        pthread_mutex_lock(&G); t->pt = pt; pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = t->handle; break;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_peek((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; break;
    }
    case 0x0A: cpu_halt(c); break;         // ExitThread: the run loop ends and thread_detach_exit finishes the job
    case 0x0B: {  // SleepThread(x0 ns); 0 / -1 / -2 are the kinds of yield
        int64_t ns = (int64_t)regs[0];
        if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
        else sched_yield();
        break;
    }
    case 0x0C: { pthread_mutex_lock(&G); thr_t *t = h_peek((uint32_t)regs[1], O_THREAD);
        regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G); break; }
    case 0x0D: { pthread_mutex_lock(&G); thr_t *t = h_peek((uint32_t)regs[0], O_THREAD);   // SetThreadPriority
        if (t) t->prio = (int)regs[1];
        regs[0] = t ? 0 : R_BAD_HANDLE; pthread_mutex_unlock(&G); break; }
    case 0x0E: { pthread_mutex_lock(&G); thr_t *t = h_peek((uint32_t)regs[2], O_THREAD);   // GetThreadCoreMask(→w1 ideal core, x2 mask)
        regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; pthread_mutex_unlock(&G); break; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 ideal core, x2 mask): -2 = process default (core 0, mask of that core), -1 = don't care, -3 = keep the ideal core
        pthread_mutex_lock(&G);
        thr_t *t = h_peek((uint32_t)regs[0], O_THREAD);
        if (t) { int cc = (int)regs[1]; if (cc >= 0) t->core = cc; if (cc == -2) { t->core = 0; t->mask = 1; } else if (regs[2]) t->mask = regs[2];
            // The ideal core must be in the mask: move it to the lowest core of the mask, as Horizon does
            if (t->mask && (t->core < 0 || !(t->mask >> t->core & 1))) t->core = __builtin_ctzll(t->mask); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; break;
    }
    case 0x10: { thr_t *t = cur(); int cc = t->core;   // always 0..2, never negative
                 regs[0] = (uint64_t)(uint32_t)(cc >= 0 ? cc : t->mask ? __builtin_ctzll(t->mask) : 0); break; }
    case 0x25: { pthread_mutex_lock(&G); thr_t *t = h_peek((uint32_t)regs[1], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; pthread_mutex_unlock(&G); break; }

    case 0x1A: {  // ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; break;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; break;   // ArbitrateUnlock(x0 address)
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        // Order matters: set the condition key = 1 first, then release the lock. The SDK's InternalConditionVariable::Signal reads the key in user mode and
        // skips svc 0x1D when it is 0; releasing first lets another thread take the lock, change the condition, read key 0, wake nobody, and this thread
        // would then wait forever. Linking into the wait chain happens inside block, still holding G: SignalProcessWideKey takes G, so it cannot land before.
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr);
        int64_t r = block(t, 2, (int64_t)regs[3]);   // on timeout the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; break;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; break;
    }
    case 0x34: {  // WaitForAddress(x0 address, w1 type, x2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal, 3 wait if the 64-bit value is equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 3 ? ld64(addr) == regs[2] : type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; break;
    }
    case 0x35: {  // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                // Waking all (count <= 0) with waiters subtracts 2, not 1 (mesosphere KAddressArbiter::SignalAndModifyByWaitingCountIfEqual)
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0; for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; break;
    }

    case 0x45: {  // CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; break;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(e); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; break;
    }
    case 0x18: {  // WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t t0 = now.tv_sec * 1000000000ll + now.tv_nsec;
        pthread_mutex_lock(&G);
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : t0 + ns - (now.tv_sec * 1000000000ll + now.tv_nsec);
            if (ns >= 0 && left <= 0) { r = R_TIMEOUT; break; }
            t->whs = hs; t->whn = n; int64_t br = block(t, 4, left); t->whs = NULL; t->whn = 0;
            if (br == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; break;
    }
    case 0x19: {  // CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_peek((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; break;
    }
    case 0x16: {  // CloseHandle(w0): an event is freed when both ends are closed; a thread object when it is closed and has exited
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;
        int64_t r = 0;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        } else r = R_BAD_HANDLE;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; break;
    }
    default: handled = 0;
    }
    if (handled) for (int i = 0; i < 4; i++) cpu_set_x(c, i, regs[i]);
    return handled;
}
