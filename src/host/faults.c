#define _GNU_SOURCE
#include "faults.h"
#include "dm.h"
#include "cpu.h"
#include <signal.h>
#include <ucontext.h>

#define MAX_HOOKS 4
static int (*g_hooks[MAX_HOOKS])(void *addr, int is_write, void *uc);
static int g_nhooks;
static void (*g_prep)(void *addr, size_t len);
static struct sigaction g_prev_segv, g_prev_bus;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void handler(int sig, siginfo_t *si, void *uc) {
    int is_write = 0;
#if defined(__x86_64__)
    is_write = (((ucontext_t *)uc)->uc_mcontext.gregs[REG_ERR] & 2) != 0;     // page-fault error code, bit 1: the access was a write
#endif
    for (int i = 0; i < g_nhooks; i++) if (g_hooks[i](si->si_addr, is_write, uc)) return;
    struct sigaction *prev = sig == SIGBUS ? &g_prev_bus : &g_prev_segv;
    if (prev->sa_flags & SA_SIGINFO) { prev->sa_sigaction(sig, si, uc); return; }
    if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) { prev->sa_handler(sig); return; }
    signal(sig, SIG_DFL);      // nobody handles it: the faulting instruction runs again and the process dies as usual
}

void fault_install(void) {
    pthread_mutex_lock(&g_mu);
    struct sigaction cur;
    for (int k = 0; k < 2; k++) {
        int sig = k ? SIGBUS : SIGSEGV;
        sigaction(sig, NULL, &cur);
        if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == handler) continue;       // ours is still on top
        struct sigaction sa = {0};
        sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, k ? &g_prev_bus : &g_prev_segv);
    }
    pthread_mutex_unlock(&g_mu);
}

void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    pthread_mutex_lock(&g_mu);
    if (g_nhooks < MAX_HOOKS) g_hooks[g_nhooks++] = fn;
    pthread_mutex_unlock(&g_mu);
    fault_install();
    cpu_set_jit_created_hook(fault_install);      // a Jit created later puts dynarmic's handler back on top
}
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { g_prep = fn; }
void dm_guest_write_prepare(void *addr, size_t len) { if (g_prep) g_prep(addr, len); }
