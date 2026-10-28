// Guest program for tests/test_kernel.cpp: threads, a kernel-arbitrated mutex, a condition variable,
// WaitForAddress/SignalToAddress and events, all through raw svc instructions. Results go to g_res[].
typedef unsigned long u64;
typedef unsigned int u32;

#define SVC(num, a0, a1, a2, a3, r0, r1, r2) do { \
    register u64 x0 __asm__("x0") = (u64)(a0), x1 __asm__("x1") = (u64)(a1), x2 __asm__("x2") = (u64)(a2), x3 __asm__("x3") = (u64)(a3); \
    __asm__ volatile("svc %[n]" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : [n] "i"(num) : "memory", "x4", "x5", "x6", "x7"); \
    r0 = x0; r1 = x1; r2 = x2; } while (0)

#define SVC_CREATE_THREAD(entry, arg, sp, prio, core, res, handle) do { \
    register u64 x0 __asm__("x0"), x1 __asm__("x1") = (u64)(entry), x2 __asm__("x2") = (u64)(arg), x3 __asm__("x3") = (u64)(sp); \
    register u64 x4 __asm__("x4") = (prio), x5 __asm__("x5") = (core); \
    __asm__ volatile("svc #0x08" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory"); \
    res = x0; handle = (u32)x1; } while (0)

#define WAITERS 0x40000000u
#define NS_MS 1000000L

u64 g_res[32];
static u32 g_stacks_unused;
char g_stacks[8][0x4000] __attribute__((aligned(16)));

static void exit_thread(void) { u64 a, b, c; SVC(0x0A, 0, 0, 0, 0, a, b, c); for (;;) {} }
static void sleep_ms(long ms) { u64 a, b, c; SVC(0x0B, ms * NS_MS, 0, 0, 0, a, b, c); }
static void mutex_lock(u32 *m, u32 tag) {
    for (;;) {
        u32 v = __atomic_load_n(m, __ATOMIC_ACQUIRE);
        if (v == 0) { if (__atomic_compare_exchange_n(m, &v, tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return; continue; }
        if (!(v & WAITERS)) { u32 nv = v | WAITERS; if (!__atomic_compare_exchange_n(m, &v, nv, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) continue; v = nv; }
        u64 a, b, c; SVC(0x1A, v & ~WAITERS, m, tag, 0, a, b, c);          // ArbitrateLock(owner, address, tag)
        if ((__atomic_load_n(m, __ATOMIC_ACQUIRE) & ~WAITERS) == tag) return;
    }
}
static void mutex_unlock(u32 *m, u32 tag) {
    u32 v = tag;
    if (__atomic_compare_exchange_n(m, &v, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    u64 a, b, c; SVC(0x1B, m, 0, 0, 0, a, b, c);                              // ArbitrateUnlock(address)
}
static u64 wait_handles(const u32 *hs, int n, long timeout_ns, u64 *index) { u64 r, i, c; SVC(0x18, 0, hs, n, timeout_ns, r, i, c); *index = i; return r; }

// ---- test A: N workers increment a plain counter under the mutex
static u32 g_mutex; static u64 g_counter;
#define WORKERS 4
#define ITERS 3000
static void worker(u64 id) {
    for (int i = 0; i < ITERS; i++) { mutex_lock(&g_mutex, 0x100 + id); g_counter++; mutex_unlock(&g_mutex, 0x100 + id); }
    exit_thread();
}
// ---- test B: condition variable
static u32 g_cv_mutex, g_cv_key, g_flag; static u64 g_consumer_got;
static void consumer(u64 id) {
    mutex_lock(&g_cv_mutex, 0x200);
    while (!g_flag) { u64 r, b, c; SVC(0x1C, &g_cv_mutex, &g_cv_key, 0x200, -1L, r, b, c); }   // WaitProcessWideKeyAtomic
    g_consumer_got = 1;
    mutex_unlock(&g_cv_mutex, 0x200);
    exit_thread();
}
// ---- test C: wait on an address
static u32 g_addr; static u64 g_addr_got;
static void addr_waiter(u64 id) {
    u64 r, b, c; SVC(0x34, &g_addr, 2, 0, -1L, r, b, c);                       // WaitForAddress: wait while the value equals 0
    g_res[11] = r; g_res[12] = g_addr;
    g_addr_got = r == 0 && g_addr == 1;
    exit_thread();
}
// ---- test D: an event signalled by another thread
static u32 g_ev_w, g_ev_r;
static void signaller(u64 id) { sleep_ms(15); u64 r, b, c; SVC(0x11, g_ev_w, 0, 0, 0, r, b, c); exit_thread(); }

void guest_main(u64 arg) {
    u64 r, i, c; u32 h[WORKERS + 1], t;

    for (int k = 0; k < WORKERS; k++) {
        SVC_CREATE_THREAD(worker, k, g_stacks[k] + 0x4000, 44, 0, r, h[k]);
        SVC(0x09, h[k], 0, 0, 0, r, i, c);
    }
    mutex_lock(&g_mutex, 0x99);                     // main competes for the same lock
    g_counter += 1000; mutex_unlock(&g_mutex, 0x99);
    for (int k = 0; k < WORKERS; k++) { r = wait_handles(&h[k], 1, -1, &i); if (r) g_res[10] = r; }
    g_res[0] = g_counter;

    SVC_CREATE_THREAD(consumer, 0, g_stacks[4] + 0x4000, 44, 1, r, t);
    SVC(0x09, t, 0, 0, 0, r, i, c);
    sleep_ms(30);                                   // let it block on the condition
    mutex_lock(&g_cv_mutex, 0x201); g_flag = 1; mutex_unlock(&g_cv_mutex, 0x201);
    SVC(0x1D, &g_cv_key, -1L, 0, 0, r, i, c);       // SignalProcessWideKey(all)
    g_res[1] = wait_handles(&t, 1, 2000 * NS_MS, &i);
    g_res[2] = g_consumer_got;

    SVC_CREATE_THREAD(addr_waiter, 0, g_stacks[5] + 0x4000, 44, 2, r, t);
    SVC(0x09, t, 0, 0, 0, r, i, c);
    sleep_ms(30);
    __atomic_store_n(&g_addr, 1, __ATOMIC_SEQ_CST);
    SVC(0x35, &g_addr, 0, 0, 1, r, i, c);           // SignalToAddress(wake 1)
    g_res[3] = wait_handles(&t, 1, 2000 * NS_MS, &i);
    g_res[4] = g_addr_got;

    SVC(0x45, 0, 0, 0, 0, r, g_ev_w, g_ev_r);        // CreateEvent
    g_res[5] = wait_handles(&g_ev_r, 1, 10 * NS_MS, &i);                         // unsignaled: times out
    SVC_CREATE_THREAD(signaller, 0, g_stacks[6] + 0x4000, 44, 0, r, t);
    SVC(0x09, t, 0, 0, 0, r, i, c);
    g_res[6] = wait_handles(&g_ev_r, 1, 2000 * NS_MS, &i);                       // signalled by the thread
    g_res[7] = i;
    SVC(0x12, g_ev_w, 0, 0, 0, r, i, c);            // ClearEvent
    g_res[8] = wait_handles(&g_ev_r, 1, 0, &i);      // cleared again: immediate timeout
    g_res[9] = 1;                                    // reached the end
    exit_thread();
}
