#define _GNU_SOURCE
#include "kernel.h"
#include "guest_mem.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/uio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <semaphore.h>

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
    int want; uint64_t ticket, since_ns;   // scheduler: asking for its core; when it asked; when it got the core
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

uint64_t kernel_tls(void);
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
static thr_t *cur(void);
uint64_t kernel_tls(void) { return cur()->tlr; }
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
// ---------- Cores ----------
// Horizon runs one thread per core at a time: the highest priority one (a lower number), the one that asked first among equals, until it blocks. Threads of one core therefore never run
// at once, and a title's startup relies on it (a worker created at lower priority does not touch what its creator still sets up). A thread gives its core up when it blocks and asks for
// it again when it wakes. At every SVC a thread yields to a waiting thread of higher priority, and to one of equal priority only after a 10 ms slice. NX_NO_SCHED=1 turns this off.
static int g_sched = -1;
static thr_t *g_owner[4];
// One condition variable per core: a core being freed wakes the threads that wait for that core, not every waiter of the four (each of which took G and walked the whole thread list to go back to sleep)
static pthread_cond_t g_core_cv[4] = {PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER};
static void core_wake_all(void) { for (int i = 0; i < 4; i++) pthread_cond_broadcast(&g_core_cv[i]); }      // a thread's core changed while it may wait on the old one (holding G)
static uint64_t g_ticket;
static _Atomic int g_nwant;
static uint64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static int sched_on(void) { if (g_sched < 0) g_sched = getenv("NX_NO_SCHED") == NULL; return g_sched; }
static void core_release(thr_t *t) {                    // holding G
    if (!sched_on()) return;
    if (g_owner[t->core & 3] == t) { g_owner[t->core & 3] = NULL; pthread_cond_broadcast(&g_core_cv[t->core & 3]); }
}
static int core_first(thr_t *t) {                       // t is the waiter that should get the core (holding G)
    for (thr_t *o = g_all; o; o = o->all)
        if (o != t && o->want && (o->core & 3) == (t->core & 3) && (o->prio < t->prio || (o->prio == t->prio && o->ticket < t->ticket))) return 0;
    return 1;
}
static void core_acquire(thr_t *t) {                    // holding G
    if (!sched_on()) return;
    int c = t->core & 3;
    if (g_owner[c] == t) return;                         // already the owner (an IPC that blocked inside took the core back)
    t->want = 1; t->ticket = ++g_ticket; atomic_fetch_add(&g_nwant, 1);
    while (g_owner[c] || !core_first(t)) { pthread_cond_wait(&g_core_cv[c], &G); c = t->core & 3; }
    g_owner[c] = t; t->want = 0; t->since_ns = mono_ns(); atomic_fetch_sub(&g_nwant, 1);
}
static void core_yield(thr_t *t) {                      // holding G
    if (!sched_on() || !atomic_load(&g_nwant)) return;
    uint64_t now = mono_ns();
    for (thr_t *o = g_all; o; o = o->all)
        if (o->want && (o->core & 3) == (t->core & 3) && (o->prio < t->prio || (o->prio == t->prio && now - t->since_ns > 10000000ull))) { core_release(t); core_acquire(t); return; }
}

static int64_t block_raw(thr_t *t, int kind, int64_t ns);
// Suspend until woken or timed out (ns < 0 infinite, 0 immediate timeout); returns the wake result or R_TIMEOUT (holding G); the core is free while it waits
static int64_t block(thr_t *t, int kind, int64_t ns) {
    if (ns == 0) return block_raw(t, kind, ns);
    core_release(t);
    int64_t r = block_raw(t, kind, ns);
    core_acquire(t);
    return r;
}
static int64_t block_raw(thr_t *t, int kind, int64_t ns) {
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

cpu_t *kernel_cpu(void) { thr_t *t = pthread_getspecific(g_keythr); return t ? t->cpu : NULL; }
uint32_t kernel_event_new_pair(uint32_t *w, uint32_t *r) {
    event_t *e = calloc(1, sizeof *e); e->refs = 2;
    pthread_mutex_lock(&G); *w = h_new(O_EVENT_W, e); *r = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
    return *r;
}
void kernel_event_clear(uint32_t h) {
    pthread_mutex_lock(&G); event_t *e = h_peek(h, O_EVENT_R); if (e) e->signaled = 0; pthread_mutex_unlock(&G);
}
uint32_t kernel_event_new(int signaled) {
    event_t *e = calloc(1, sizeof *e); e->refs = 1; e->signaled = signaled;
    pthread_mutex_lock(&G); uint32_t h = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
    return h;
}
void kernel_event_signal(uint32_t h) {
    pthread_mutex_lock(&G);
    event_t *e = h_peek(h, O_EVENT_R);
    if (e) { e->signaled = 1; wake_sync_waiters(e); }
    pthread_mutex_unlock(&G);
}

// ---------- Threads ----------
const char *hle_stub_name(uint32_t swi);
static void on_svc(cpu_t *c, uint32_t swi, void *user);
static void on_fault(cpu_t *c, uint64_t pc, int kind, void *user);
static uint64_t cntpct_cb(void) { return kernel_cntpct(); }
extern int hle_dispatch(cpu_t *c, uint32_t swi);      // hle_stub.c
extern int mem_svc(cpu_t *c, uint32_t swi);           // mem.c
extern int ipc_svc(cpu_t *c, uint32_t swi);           // ipc.c

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
    t->exited = 1; core_release(t);
    wake_sync_waiters(t);                               // WaitSynchronization on this thread re-checks
    pthread_cond_broadcast(&g_exit_cv);
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
}

static void run_guest(thr_t *t) {
    cpu_set_pc(t->cpu, t->entry); cpu_set_sp(t->cpu, t->stack_top);
    cpu_set_x(t->cpu, 0, t->arg); cpu_set_x(t->cpu, 1, t->handle); cpu_set_x(t->cpu, 30, g_landing);   // x1: the main thread's handle, which Horizon gives rtld (_start(0, handle))
    cpu_run(t->cpu);
}

static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    thread_attach(t);
    pthread_mutex_lock(&G); core_acquire(t); pthread_mutex_unlock(&G);       // wait for the core before running guest code
    run_guest(t);
    thread_detach_exit(t);
    return NULL;
}

// What every guest thread is doing: priority, core, what it waits on, lr, and (stacks) the guest call stack. Taking G could itself hang if the freeze being looked at holds it, so it is
// tried for two seconds and the dump goes on without it (said in the header line) when it stays taken: the thread list is only read.
static void threads_dump_once(int stacks, int all, const char *why) {
    int locked = 0; for (int i = 0; i < 200 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(10000);
    fprintf(stderr, "[threads] --- %s%s\n", why, locked ? "" : " (the kernel lock is held by someone: reading without it)");
    for (int c = 0; c < 4; c++) if (g_owner[c]) fprintf(stderr, "[threads] core %d is held by %#llx (%.1f s)\n", c, (unsigned long long)g_owner[c]->id, (double)(mono_ns() - g_owner[c]->since_ns) / 1e9);
    for (thr_t *t = g_all; t; t = t->all)
        fprintf(stderr, "[threads] id=%#llx h=%#x prio=%d core=%d %s%s wait=%s addr=%#llx lr=%#llx pc=%#llx r0=%#llx\n", (unsigned long long)t->id, t->handle, t->prio, t->core, t->exited ? "EXITED" : "alive", t->want ? " wants-core" : "",
                (const char *[]){"none", "lock", "cond", "address", "sync"}[t->wkind >= 0 && t->wkind < 5 ? t->wkind : 0], (unsigned long long)t->waddr, t->cpu ? (unsigned long long)cpu_get_x(t->cpu, 30) : 0ull,
                t->cpu ? (unsigned long long)cpu_get_pc(t->cpu) : 0ull, t->cpu ? (unsigned long long)cpu_get_x(t->cpu, 0) : 0ull);
    if (stacks)
        for (thr_t *t = g_all; t; t = t->all) if (t->cpu && !t->exited && (t->wkind == 0 || all)) {
            uint64_t sp = cpu_get_sp(t->cpu); fprintf(stderr, "[threads]   %#llx stack:", (unsigned long long)t->id);
            uint32_t buf[160]; struct iovec lo = {buf, sizeof buf}, re = {(void *)(uintptr_t)sp, sizeof buf};
            ssize_t got = process_vm_readv(getpid(), &lo, 1, &re, 1, 0);       // a stack may end before 640 bytes: read what is mapped
            if (cpu_is_aarch32()) { for (int i = 0; i < (got > 0 ? got / 4 : 0); i++) if (buf[i] >= 0x206000 && buf[i] < 0x1512000) fprintf(stderr, " %#x", buf[i]); }
            else for (int i = 0; i + 1 < (got > 0 ? got / 4 : 0); i += 2) { uint64_t v = buf[i] | (uint64_t)buf[i + 1] << 32; if (v >= 0x2000011000ull && v < 0x2001a63000ull) fprintf(stderr, " %#llx", (unsigned long long)v - 0x2000015000ull); }       // 64
            fprintf(stderr, "\n");
        }
    if (locked) pthread_mutex_unlock(&G);
}

static void *threads_dump(void *unused) {            // NX_THREADS=<seconds>: the dump above, repeated (NX_THREADS_STACK=1 adds the stacks, NX_THREADS_ALL=1 also those of blocked threads)
    (void)unused; int secs = atoi(getenv("NX_THREADS")); if (secs < 1) secs = 10;
    for (;;) { sleep((unsigned)secs); threads_dump_once(getenv("NX_THREADS_STACK") != NULL, getenv("NX_THREADS_ALL") != NULL, "every NX_THREADS seconds"); }
    return NULL;
}

// kill -QUIT <pid> (Ctrl-\ in the terminal) dumps every guest thread with its stack, blocked ones too, into stderr/the log, in a process started without any variable: a frozen title can be looked at.
// The handler only posts the semaphore (async-signal-safe); a thread does the printing. The process no longer dies of SIGQUIT: use SIGTERM or SIGKILL. NX_NO_QUIT_DUMP=1 leaves SIGQUIT alone.
static sem_t g_dump_sem;
static void on_quit(int sig) { (void)sig; sem_post(&g_dump_sem); }
static void *quit_dump(void *unused) {
    (void)unused;
    for (;;) { if (sem_wait(&g_dump_sem)) continue; threads_dump_once(1, 1, "SIGQUIT"); }
    return NULL;
}
static void quit_dump_start(void) {
    if (getenv("NX_NO_QUIT_DUMP") || sem_init(&g_dump_sem, 0, 0)) return;
    pthread_t th; if (pthread_create(&th, NULL, quit_dump, NULL)) return;
    pthread_detach(th);
    struct sigaction sa = {0}; sa.sa_handler = on_quit; sa.sa_flags = SA_RESTART; sigemptyset(&sa.sa_mask); sigaction(SIGQUIT, &sa, NULL);
}

void kernel_init(void) {
    pthread_key_create(&g_keythr, NULL);
    if (getenv("NX_THREADS")) { pthread_t th; pthread_create(&th, NULL, threads_dump, NULL); pthread_detach(th); }
    quit_dump_start();
    g_landing = guest_alloc(16, 16);
    if (cpu_is_aarch32()) {                                  // where a guest function the host calls (cpu_call32) returns to
        uint64_t pad = guest_alloc(16, 16); *(uint32_t *)pad = 0xEF0007FEu;
        extern void cpu32_set_return_pad(uint32_t);
        cpu32_set_return_pad((uint32_t)pad);
    }
    if (!cpu_is_aarch32()) { uint64_t pad = guest_alloc(16, 16); ((uint32_t *)pad)[0] = 0xD4000001u | (0x7FEu << 5); ((uint32_t *)pad)[1] = 0xD65F03C0u; cpu_set_return_pad64(pad); }   // svc #0x7FE: where a guest function the host calls (cpu_call64) returns to
    *(uint32_t *)g_landing = cpu_is_aarch32() ? 0xEF000000u | SVC_THREAD_RETURNED           // svc #SVC_THREAD_RETURNED
                                              : 0xD4000001u | (SVC_THREAD_RETURNED << 5);
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
    pthread_mutex_lock(&G); core_acquire(t); pthread_mutex_unlock(&G);
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

// An IPC request blocks the caller until the service answers, so on a console a thread that polls a service without ever sleeping (MK8D's vibration thread, priority 41) does not keep
// its core: lower priority threads of that core run meanwhile. The host answers inside the SVC, so the core is given up for the duration when another thread waits for it.
static int ipc_svc_yielding(cpu_t *c, uint32_t swi) {
    thr_t *me = pthread_getspecific(g_keythr); int rel = 0;
    if (me && (swi == 0x21 || swi == 0x22) && sched_on() && atomic_load(&g_nwant)) { pthread_mutex_lock(&G); if (g_owner[me->core & 3] == me) { core_release(me); rel = 1; } pthread_mutex_unlock(&G); }
    int r = ipc_svc(c, swi);
    if (rel) { pthread_mutex_lock(&G); core_acquire(me); pthread_mutex_unlock(&G); }
    return r;
}

static void on_svc(cpu_t *c, uint32_t swi, void *user) {
    (void)user;
    if (!cpu_is_aarch32()) { static int tr = -1; static const char *tf; static _Atomic int n; if (tr < 0) { tf = getenv("NX_TRACE_SVC"); tr = tf != NULL; }       // NX_TRACE_SVC for an AArch64 title: the first 3000 SVCs (read once: this runs on every SVC)
        int match = tf && tf[0] && strcmp(tf, "1") && swi >= 0x1000 && hle_stub_name(swi) && strstr(hle_stub_name(swi), tf);       // NX_TRACE_SVC=<name part>: only hooks whose name has it
        if (match || (tr && (!tf || !strcmp(tf, "1")) && n++ < 3000)) fprintf(stderr, "[svc] %#x%s%s x0=%#llx x1=%#llx x2=%#llx x3=%#llx lr=%#llx\n", swi, swi >= 0x1000 && hle_stub_name(swi) ? " " : "", swi >= 0x1000 && hle_stub_name(swi) ? hle_stub_name(swi) : "", (unsigned long long)cpu_get_x(c, 0), (unsigned long long)cpu_get_x(c, 1), (unsigned long long)cpu_get_x(c, 2), (unsigned long long)cpu_get_x(c, 3), (unsigned long long)cpu_get_x(c, 30)); }
    { thr_t *me = pthread_getspecific(g_keythr); if (me && atomic_load(&g_nwant)) { pthread_mutex_lock(&G); core_yield(me); pthread_mutex_unlock(&G); } }   // a yield point of the core scheduler
    static int errs = -1; if (errs < 0) errs = getenv("NX_SVC_ERRORS") != NULL;       // NX_SVC_ERRORS=1: every distinct (svc, non-zero result) once, with its arguments
    uint64_t a0 = errs ? cpu_get_x(c, 0) : 0, a1 = errs ? cpu_get_x(c, 1) : 0, a2 = errs ? cpu_get_x(c, 2) : 0, a3 = errs ? cpu_get_x(c, 3) : 0, lr = errs ? cpu_get_x(c, 30) : 0;
    #define SVC_DONE(call) if (call) { if (errs && swi < 0x100) { uint32_t r = (uint32_t)cpu_get_x(c, 0); if (r) { static uint64_t seen[256]; static int ns; uint64_t key = (uint64_t)swi << 32 | r; int dup = 0; for (int i = 0; i < ns; i++) if (seen[i] == key) dup = 1; if (!dup && ns < 256) { seen[ns++] = key; fprintf(stderr, "[svcerr] svc %#x -> %#x (x0=%#llx x1=%#llx x2=%#llx x3=%#llx lr=%#llx)\n", swi, r, (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)lr); } } } return; }
    SVC_DONE(ipc_svc_yielding(c, swi))             // before the kernel: CloseHandle of a session is not a kernel handle
    SVC_DONE(kernel_svc(c, swi))
    SVC_DONE(mem_svc(c, swi))
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
        { static int ms = -1; if (ms < 0) { const char *e = getenv("NX_START_DELAY_MS"); ms = e ? atoi(e) : 0; } if (ms > 0) { struct timespec d = {0, ms * 1000000L}; nanosleep(&d, NULL); } }   // debugging: a title that races its own init
        pthread_mutex_lock(&G);
        thr_t *t = h_peek((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; break;
    }
    case 0x0A: cpu_halt(c); break;         // ExitThread: the run loop ends and thread_detach_exit finishes the job
    case 0x0B: {  // SleepThread(x0 ns); 0 / -1 / -2 are the kinds of yield
        int64_t ns = (int64_t)regs[0];
        thr_t *me = cur();
        if (ns > 0) {
            pthread_mutex_lock(&G); core_release(me); pthread_mutex_unlock(&G);
            struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL);
            pthread_mutex_lock(&G); core_acquire(me); pthread_mutex_unlock(&G);
        } else {                                               // a yield: to any waiter of the core, not only the higher priority ones
            pthread_mutex_lock(&G);
            for (thr_t *o = g_all; o && sched_on(); o = o->all) if (o->want && (o->core & 3) == (me->core & 3) && o->prio <= me->prio) { core_release(me); core_acquire(me); break; }
            pthread_mutex_unlock(&G);
            sched_yield();
        }
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
            if (t->mask && (t->core < 0 || !(t->mask >> t->core & 1))) t->core = __builtin_ctzll(t->mask);
            core_wake_all(); }
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

// A host function that blocks the guest thread outside the kernel (a condition wait in a hook) gives its core up for the time and takes it back
void kernel_core_release(void) { thr_t *t = pthread_getspecific(g_keythr); if (t) { pthread_mutex_lock(&G); core_release(t); pthread_mutex_unlock(&G); } }
void kernel_core_acquire(void) { thr_t *t = pthread_getspecific(g_keythr); if (t) { pthread_mutex_lock(&G); core_acquire(t); pthread_mutex_unlock(&G); } }
