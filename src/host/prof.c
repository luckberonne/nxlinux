// NX_PROF=<period in microseconds, default 4000>: a sampling profiler of the host, for the machines where perf is not at hand (ptrace is limited, so nothing can attach from outside).
// A thread looks at /proc/self/task every period and signals each thread that is running (state R); the handler of that thread stores its program counter and a few return addresses.
// kill -USR2 <pid> writes the samples to NX_PROF_OUT (default nx_prof.txt in the working directory) and starts over; tools/prof_report.py turns the file into a table by function.
#define _GNU_SOURCE
#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

enum { NF = 12, NSAMP = 1 << 18 };
static struct { int tid, n; void *f[NF]; } *g_s;
static _Atomic unsigned g_n;
static volatile sig_atomic_t g_dump;
static int g_period = 4000;

static void on_prof(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    unsigned i = atomic_fetch_add(&g_n, 1);
    if (i >= NSAMP) return;
    void *bt[NF + 2]; int n = backtrace(bt, NF + 2);
    g_s[i].tid = (int)syscall(SYS_gettid);
    g_s[i].f[0] = (void *)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
    int k = 1; for (int j = 2; j < n && k < NF; j++) g_s[i].f[k++] = bt[j];      // bt[0] is this handler and bt[1] the signal trampoline
    g_s[i].n = k;
}
static void on_usr2(int sig) { (void)sig; g_dump = 1; }

static void dump(void) {
    const char *path = getenv("NX_PROF_OUT"); if (!path) path = "nx_prof.txt";
    FILE *o = fopen(path, "w"); if (!o) return;
    FILE *m = fopen("/proc/self/maps", "r"); char line[512], exe[512] = ""; ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1); if (r > 0) exe[r] = 0;
    while (m && fgets(line, sizeof line, m)) {
        unsigned long a, b, off; char perm[8]; if (sscanf(line, "%lx-%lx %7s %lx", &a, &b, perm, &off) != 4) continue;
        if (strstr(line, exe) && strstr(line, " 00000000 ")) fprintf(o, "base %lx\n", a);
        if (perm[2] == 'x') { char *nm = strchr(line, '/'); if (!nm) nm = strstr(line, "[") ; fprintf(o, "M %lx %lx %lx %s", a, b, off, nm ? nm : "anon\n"); }   // executable mappings: which module an address belongs to
    }
    if (m) fclose(m);
    DIR *d = opendir("/proc/self/task");
    for (struct dirent *e; d && (e = readdir(d));) if (e->d_name[0] != '.') { char p[96], c[64] = ""; snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name); FILE *f = fopen(p, "r"); if (f) { if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0; fclose(f); } fprintf(o, "T %s %s\n", e->d_name, c); }
    if (d) closedir(d);
    unsigned n = atomic_load(&g_n); if (n > NSAMP) n = NSAMP;
    for (unsigned i = 0; i < n; i++) { fprintf(o, "S %d", g_s[i].tid); for (int k = 0; k < g_s[i].n; k++) fprintf(o, " %lx", (unsigned long)g_s[i].f[k]); fprintf(o, "\n"); }
    fclose(o);
    atomic_store(&g_n, 0);
    fprintf(stderr, "[dm] profile: %u samples written to %s\n", n, path);
}

static void *sampler(void *arg) {
    (void)arg;
    sigset_t all; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, NULL);           // this thread is never interrupted
    pid_t pid = getpid(), me = (pid_t)syscall(SYS_gettid);
    for (;;) {
        usleep((useconds_t)g_period);
        if (g_dump) { g_dump = 0; dump(); continue; }
        DIR *d = opendir("/proc/self/task"); if (!d) continue;
        for (struct dirent *e; (e = readdir(d));) {
            if (e->d_name[0] == '.') continue;
            int tid = atoi(e->d_name); if (tid == me) continue;
            char p[96], buf[256]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
            FILE *f = fopen(p, "r"); if (!f) continue;
            char *s = fgets(buf, sizeof buf, f); fclose(f);
            if (!s) continue;
            char *rp = strrchr(buf, ')'); if (rp && rp[1] == ' ' && rp[2] == 'R') syscall(SYS_tgkill, pid, tid, SIGPROF);
        }
        closedir(d);
    }
    return NULL;
}

void nx_prof_init(void) {
    const char *e = getenv("NX_PROF"); if (!e) return;
    if (atoi(e) > 0) g_period = atoi(e);
    g_s = calloc(NSAMP, sizeof *g_s); if (!g_s) return;
    void *warm[4]; backtrace(warm, 4);                                                // the first call loads the unwinder: not from a signal handler
    struct sigaction sa = {0}; sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask); sigaction(SIGPROF, &sa, NULL);
    struct sigaction su = {0}; su.sa_handler = on_usr2; su.sa_flags = SA_RESTART; sigemptyset(&su.sa_mask); sigaction(SIGUSR2, &su, NULL);
    pthread_t t; if (pthread_create(&t, NULL, sampler, NULL) == 0) pthread_detach(t);
    fprintf(stderr, "[dm] profile: sampling every %d us; kill -USR2 %d writes the samples\n", g_period, (int)getpid());
}
