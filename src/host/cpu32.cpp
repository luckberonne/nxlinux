// AArch32 guest CPUs, the counterpart of cpu.cpp: a pool of dynarmic A32 Jits that guest threads borrow while they run guest code. The guest address space is the low 4 GiB, where
// guest address == host address, so the Jit's fastmem base is 0. TLS is in cp15 (TPIDRURO holds the ThreadLocalRegion, TPIDRURW is the SDK's); the system counter is cp15's CNTPCT.
#include "cpu32.h"
#include "svc32.h"
extern "C" const char *hle_stub_name(uint32_t swi) __attribute__((weak));   // hle_stub.c; absent in the CPU-only tests
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <sys/mman.h>
#include <signal.h>
#include <map>
#include <algorithm>
#include <thread>
#include <chrono>

using namespace Dynarmic::A32;
using Dynarmic::ExclusiveMonitor;
using Dynarmic::HaltReason;

namespace {

constexpr uint32_t CPSR_T = 1u << 5;
// NX_WATCH=<guest address>: find out who writes a variable. Its page is made read-only, so the Jit's fastmem store faults and lands in MemoryWrite*; wr() then does the write, and when it
// touches the watched word it prints the value and stops the thread (HaltExecution(MemoryAbort) saves the pc of the store) to report where the guest is. A debugging aid: host code that
// writes the same page (the kernel's lock words) would fault, so choose a variable in a data page the guest keeps to itself.
uint32_t g_watch, g_watch_page; std::mutex g_watch_mu;
// NX_TRACE_PC=<file>: every instruction the Jit translates, in the order it is first reached (a u32 address each, written when the process ends): the discovery order of the guest's
// code, which shows which parts of a title's startup ran and which were never reached.
std::vector<uint32_t> g_pc_log; std::mutex g_pc_mu; bool g_pc_trace;
void dump_pc_log() {
    const char *f = getenv("NX_TRACE_PC");
    if (!f || !g_pc_trace) return;
    std::lock_guard<std::mutex> lk(g_pc_mu);
    if (FILE *o = fopen(f, "wb")) { fwrite(g_pc_log.data(), 4, g_pc_log.size(), o); fclose(o); fprintf(stderr, "[trace] %zu translated instructions written to %s\n", g_pc_log.size(), f); }
}
std::map<uint32_t, uint32_t> g_trap;            // address -> the instruction a trap replaced
uint32_t trap_orig(uint32_t pc) { auto it = g_trap.find(pc); return it == g_trap.end() ? 0 : it->second; }
bool g_svc_stats = getenv("NX_SVC_STATS") != nullptr; std::mutex g_stats_mu; std::map<uint64_t, uint64_t> g_stats;
void start_stats() {
    if (!g_svc_stats) return;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::vector<std::pair<uint64_t, uint64_t>> v;
            { std::lock_guard<std::mutex> lk(g_stats_mu); for (auto &kv : g_stats) v.push_back({kv.second, kv.first}); g_stats.clear(); }
            std::sort(v.rbegin(), v.rend());
            fprintf(stderr, "[stats] ---\n");
            for (size_t i = 0; i < v.size() && i < 10; i++) {
                uint32_t n = (uint32_t)(v[i].second >> 32), lr = (uint32_t)v[i].second;
                fprintf(stderr, "[stats] %8llu  %s%#x  lr=%#x\n", (unsigned long long)v[i].first, n >= 0x1000 ? "hook " : "svc ", n, lr);
                if (n >= 0x1000 && hle_stub_name && hle_stub_name(n)) fprintf(stderr, "[stats]           %s\n", hle_stub_name(n));
            }
        }
    }).detach();
}
uint64_t g_win_lo = 0, g_win_hi = 1ull << 32;      // the guest window, once guest_mem_init32 says where it is

struct ctx {
    uint32_t r[16] = {};
    uint32_t ext[64] = {};
    uint32_t cpsr = 0x10, fpscr = 0;      // user mode
    uint32_t tpidrurw = 0, tpidruro = 0;
};

struct jitenv;

struct cpu32 {
    cpu_ops_t ops;
    void *user;
    ctx s;
    bool svc_pending = false, halt_req = false, faulted = false;
    uint32_t svc_num = 0;
    bool in_svc = false;
    uint64_t fv[8] = {}, fret = 0;         // the host function's v0..v7: the VFP arguments of a hook, and the v0 it leaves
    uint64_t vx[8] = {};                   // x0..x7 as the AArch64-layout handlers see them during an SVC
    svc32_ctx_t svc_cx;
    int nested = 0;
    bool watch_hit = false, watch_skip = false;   // the thread stopped on a watched write / its next write to the word (the re-executed store) passes silently
    uint32_t watch_value = 0;                        // depth of cpu32_call
    std::atomic<jitenv *> running{nullptr};
};

struct jitenv;

struct Cp15 final : Coprocessor {
    jitenv *env;
    explicit Cp15(jitenv *e) : env(e) {}
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override;
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override;
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override;
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
};

struct jitenv final : UserCallbacks {
    std::unique_ptr<Jit> jit;
    unsigned id = 0;
    cpu32 *cur = nullptr;
    uint32_t tpidrurw = 0, tpidruro = 0;   // the Jit's cp15 TLS registers while a cpu is loaded

    // The fallback path (the Jit's fastmem fault lands here): an address outside the window is a guest bug or a missing host feature, so say where the guest last was and stop the thread
    bool ok(uint32_t a, size_t n) {
        if ((uint64_t)a >= g_win_lo && (uint64_t)a + n <= g_win_hi) return true;
        fprintf(stderr, "[RED] guest access to unmapped address %#x (last svc %#x at pc=%#x lr=%#x, r0=%#x r1=%#x sp=%#x)\n", a, cur->svc_num, cur->s.r[15], cur->s.r[14], cur->s.r[0], cur->s.r[1], cur->s.r[13]);
        fault(cur->s.r[15], 3);
        jit->HaltExecution(HaltReason::MemoryAbort);        // the one halt flag the Jit checks after a memory access (check_halt_on_memory_access); it also saves the pc
        return false;
    }
    template <class T> T rd(uint32_t a) { T v{}; if (ok(a, sizeof v)) memcpy(&v, (const void *)(uintptr_t)a, sizeof v); return v; }
    template <class T> void wr(uint32_t a, T v) {
        if (!ok(a, sizeof v)) return;
        if (g_watch_page && (a & ~0xFFFu) == g_watch_page) {
            std::lock_guard<std::mutex> lk(g_watch_mu);
            mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
            memcpy((void *)(uintptr_t)a, &v, sizeof v);
            mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ | PROT_EXEC);
            if (a < g_watch + 4 && a + sizeof v > g_watch) {
                if (cur->watch_skip) cur->watch_skip = false;
                else { cur->watch_hit = true; cur->watch_skip = true; cur->watch_value = (uint32_t)v; jit->HaltExecution(HaltReason::MemoryAbort); }
            }
            return;
        }
        memcpy((void *)(uintptr_t)a, &v, sizeof v);
    }
    template <class T> bool excl(uint32_t a, T v, T expected) {
        if (!ok(a, sizeof v)) return false;
        if (g_watch_page && (a & ~0xFFFu) == g_watch_page) {              // the watched page is read-only: open it for the compare-exchange
            std::lock_guard<std::mutex> lk(g_watch_mu);
            mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
            bool done = __atomic_compare_exchange_n((T *)(uintptr_t)a, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ | PROT_EXEC);
            if (done && a < g_watch + 4 && a + sizeof v > g_watch) {
                if (cur->watch_skip) cur->watch_skip = false;
                else { cur->watch_hit = true; cur->watch_skip = true; cur->watch_value = (uint32_t)v; jit->HaltExecution(HaltReason::MemoryAbort); }
            }
            return done;
        }
        return __atomic_compare_exchange_n((T *)(uintptr_t)a, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    uint8_t MemoryRead8(VAddr a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(VAddr a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(VAddr a) override { return rd<uint32_t>(a); }
    uint64_t MemoryRead64(VAddr a) override { return rd<uint64_t>(a); }
    void MemoryWrite8(VAddr a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(VAddr a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(VAddr a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(VAddr a, uint64_t v) override { wr(a, v); }
    bool MemoryWriteExclusive8(VAddr a, uint8_t v, uint8_t e) override { return excl(a, v, e); }
    bool MemoryWriteExclusive16(VAddr a, uint16_t v, uint16_t e) override { return excl(a, v, e); }
    bool MemoryWriteExclusive32(VAddr a, uint32_t v, uint32_t e) override { return excl(a, v, e); }
    bool MemoryWriteExclusive64(VAddr a, uint64_t v, uint64_t e) override { return excl(a, v, e); }
    void InterpreterFallback(VAddr pc, size_t) override { fault(pc, 1); }
    void ExceptionRaised(VAddr pc, Exception) override { fault(pc, 2); }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1ull << 40; }
    void PreCodeTranslationHook(bool, VAddr pc, IREmitter &) override {
        if (g_pc_trace) { std::lock_guard<std::mutex> lk(g_pc_mu); g_pc_log.push_back(pc); }
    }
    void CallSVC(uint32_t swi) override {      // leave Run(); the kernel handles the svc outside, see cpu32_run
        cur->svc_pending = true; cur->svc_num = swi;
        jit->HaltExecution(HaltReason::UserDefined2);
    }
    void fault(uint32_t pc, int kind) {
        cur->faulted = true;
        if (cur->ops.fault) cur->ops.fault(reinterpret_cast<cpu_t *>(cur), pc, kind, cur->user);
        jit->HaltExecution(HaltReason::UserDefined3);
    }
};

Coprocessor::CallbackOrAccessOneWord Cp15::CompileSendOneWord(bool, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 2) return &env->tpidrurw;
    if (opc1 == 0 && CRn == CoprocReg::C7)       // the ARMv6 barrier operations (isb, dsb, dmb): nothing to do on this host
        return Callback{[](void *, uint32_t, uint32_t) -> uint64_t { return 0; }, std::nullopt};
    return std::monostate{};
}
Coprocessor::CallbackOrAccessOneWord Cp15::CompileGetOneWord(bool, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    if (opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0) {
        if (opc2 == 2) return &env->tpidrurw;
        if (opc2 == 3) return &env->tpidruro;
    }
    return std::monostate{};
}
Coprocessor::CallbackOrAccessTwoWords Cp15::CompileGetTwoWords(bool, unsigned opc, CoprocReg CRm) {
    if (opc == 0 && CRm == CoprocReg::C14)       // mrrc p15, 0, rt, rt2, c14: CNTPCT
        return Callback{[](void *u, uint32_t, uint32_t) -> uint64_t { return static_cast<jitenv *>(u)->cur->ops.cntpct(); }, static_cast<void *>(env)};
    return std::monostate{};
}

ExclusiveMonitor &monitor() {
    static ExclusiveMonitor m(CPU_MAX_CORES);
    return m;
}

std::mutex g_mu;
std::condition_variable g_cv;
std::vector<std::unique_ptr<jitenv>> g_envs;
std::vector<jitenv *> g_free;
unsigned g_max_jits, g_created;

unsigned max_jits() {
    if (!g_max_jits) {
        const char *e = getenv("NX_JITS");
        long n = e ? atol(e) : 8;
        g_max_jits = n < 1 ? 1 : n > CPU_MAX_CORES ? CPU_MAX_CORES : (unsigned)n;
    }
    return g_max_jits;
}

// NX_WATCHDOG=1: once a second, the registers of every Jit that is running guest code (read while it runs, so only a hint of where a stuck title is)
void start_watchdog() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!getenv("NX_WATCHDOG")) return;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::lock_guard<std::mutex> lk(g_mu);
                for (auto &e : g_envs) if (e->cur) {
                    auto &r = e->jit->Regs();
                    fprintf(stderr, "[watchdog] jit %u pc=%#x lr=%#x sp=%#x r0=%#x r1=%#x r2=%#x r3=%#x cpsr=%#x\n", e->id, r[15], r[14], r[13], r[0], r[1], r[2], r[3], e->jit->Cpsr());
                }
            }
        }).detach();
    });
}

std::unique_ptr<jitenv> make_env(unsigned id) {
    start_watchdog();
    auto e = std::make_unique<jitenv>();
    e->id = id;
    UserConfig cfg{};
    cfg.callbacks = e.get();
    cfg.fastmem_pointer = 0;                    // guest address == host address, all of it below 4 GiB
    cfg.fastmem_exclusive_access = false;       // exclusives go through MemoryWriteExclusive* (compare-exchange)
    cfg.enable_cycle_counting = false;
    cfg.check_halt_on_memory_access = true;     // a bad access (ok()) halts the thread; without the check a loop that keeps reading address 0 never leaves Run()
    cfg.global_monitor = &monitor();
    cfg.processor_id = id;
    cfg.coprocessors[15] = std::make_shared<Cp15>(e.get());
    e->jit = std::make_unique<Jit>(cfg);
    return e;
}

jitenv *acquire(cpu32 *c) {
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        if (!g_free.empty()) { jitenv *e = g_free.back(); g_free.pop_back(); e->cur = c; c->running = e; return e; }
        if (g_created < max_jits()) {
            unsigned id = g_created++;          // reserved under the lock: ids are the exclusive-monitor slots and must be unique
            lk.unlock();
            auto e = make_env(id);
            lk.lock();
            jitenv *p = e.get();
            g_envs.push_back(std::move(e));
            p->cur = c; c->running = p;
            return p;
        }
        g_cv.wait(lk);
    }
}

void release(cpu32 *c, jitenv *e) {
    std::lock_guard<std::mutex> lk(g_mu);
    c->running = nullptr;
    e->jit->ClearHalt(HaltReason::UserDefined1);
    e->jit->ClearHalt(HaltReason::UserDefined2);
    e->jit->ClearHalt(HaltReason::UserDefined3);
    e->jit->ClearHalt(HaltReason::MemoryAbort);
    e->cur = nullptr;
    g_free.push_back(e);
    g_cv.notify_one();
}

cpu32 *C(cpu_t *c) { return reinterpret_cast<cpu32 *>(c); }

void load(jitenv *e, const ctx &s) {
    Jit &j = *e->jit;
    memcpy(j.Regs().data(), s.r, sizeof s.r);
    memcpy(j.ExtRegs().data(), s.ext, sizeof s.ext);
    j.SetCpsr(s.cpsr); j.SetFpscr(s.fpscr);
    e->tpidrurw = s.tpidrurw; e->tpidruro = s.tpidruro;
}
void store(jitenv *e, ctx &s) {
    Jit &j = *e->jit;
    memcpy(s.r, j.Regs().data(), sizeof s.r);
    memcpy(s.ext, j.ExtRegs().data(), sizeof s.ext);
    s.cpsr = j.Cpsr(); s.fpscr = j.Fpscr();
    s.tpidrurw = e->tpidrurw; s.tpidruro = e->tpidruro;
}

int regidx(int i) { return i == 30 ? 14 : i; }     // the kernel's x30 (lr) is r14

}  // namespace

static uint32_t g_return_pad;
void cpu32_set_return_pad(uint32_t addr) { g_return_pad = addr; }

uint32_t cpu32_call(cpu_t *cp, uint32_t fn, const uint32_t *args, int nargs) {
    cpu32 *c = C(cp);
    if (!g_return_pad) { fprintf(stderr, "[RED] cpu32_call: no return pad\n"); return 0; }
    ctx saved = c->s; bool in_svc = c->in_svc, halt = c->halt_req; uint64_t vx[8]; memcpy(vx, c->vx, sizeof vx); svc32_ctx_t cx = c->svc_cx;
    c->in_svc = false;
    if (nargs > 8) nargs = 8;
    uint32_t sp = (saved.r[13] - 0x100 - 4 * (nargs > 4 ? nargs - 4 : 0)) & ~7u;   // a frame below the caller's; the words after the fourth argument are on the stack
    for (int i = 0; i < nargs; i++) { if (i < 4) c->s.r[i] = args[i]; else *(uint32_t *)(uintptr_t)(sp + 4 * (i - 4)) = args[i]; }
    c->s.r[13] = sp;
    c->s.r[14] = g_return_pad;
    cpu32_set_pc(cp, fn);
    c->nested++;
    cpu32_run(cp);
    c->nested--;
    uint32_t ret = c->s.r[0];
    c->s = saved; c->in_svc = in_svc; c->halt_req = halt; memcpy(c->vx, vx, sizeof vx); c->svc_cx = cx;
    return ret;
}

void cpu32_set_trap(uint32_t addr) { uint32_t *p = (uint32_t *)(uintptr_t)addr; g_trap[addr] = *p; *p = 0xEF001FFE; }   // svc #0x1FFE
void cpu32_set_window(uint64_t lo, uint64_t hi) { g_win_lo = lo; g_win_hi = hi; }

static void arm_watch() {
    static std::once_flag once;
    std::call_once(once, [] {
        start_stats();
        if (getenv("NX_ZERO_FAULT")) {            // NX_ZERO_FAULT=1: a guest access to an uncommitted page of the window gets a zero page instead of killing the host (an experiment for titles running on half-built state)
            static struct sigaction old_segv;
            struct sigaction sa = {};
            sa.sa_flags = SA_SIGINFO | SA_NODEFER;
            sa.sa_sigaction = [](int sig, siginfo_t *si, void *uc) {
                uint64_t a = (uint64_t)(uintptr_t)si->si_addr;
                if (a >= g_win_lo && a < g_win_hi && (g_watch_page == 0 || (a & ~0xFFFull) != g_watch_page)) {
                    static std::atomic<int> n;
                    if (n++ < 20) fprintf(stderr, "[RED] zero page given to a guest access at %#llx\n", (unsigned long long)a);
                    if (!mprotect((void *)(uintptr_t)(a & ~0xFFFull), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC)) return;
                }
                if (old_segv.sa_flags & SA_SIGINFO) old_segv.sa_sigaction(sig, si, uc);
                else if (old_segv.sa_handler != SIG_DFL && old_segv.sa_handler != SIG_IGN) old_segv.sa_handler(sig);
                else { signal(sig, SIG_DFL); raise(sig); }
            };
            sigaction(SIGSEGV, &sa, &old_segv);
        }
        if (getenv("NX_TRACE_PC")) {
            g_pc_trace = true; atexit(dump_pc_log);
            static struct sigaction old_segv;
            struct sigaction sa = {};
            sa.sa_flags = SA_SIGINFO | SA_NODEFER;
            sa.sa_sigaction = [](int sig, siginfo_t *si, void *uc) {            // a host crash also writes the log, then goes on to whatever handler was there before
                dump_pc_log();
                if (old_segv.sa_flags & SA_SIGINFO) old_segv.sa_sigaction(sig, si, uc);
                else if (old_segv.sa_handler != SIG_DFL && old_segv.sa_handler != SIG_IGN) old_segv.sa_handler(sig);
                else { signal(sig, SIG_DFL); raise(sig); }
            };
            sigaction(SIGSEGV, &sa, &old_segv);
            if (const char *after = getenv("NX_TRACE_PC_AFTER")) {            // also write the log after this many seconds: a title that ends in a host crash never exits normally
                int secs = atoi(after);
                std::thread([secs] { std::this_thread::sleep_for(std::chrono::seconds(secs)); dump_pc_log(); }).detach();
            }
        }
        const char *e = getenv("NX_WATCH");
        if (!e) return;
        g_watch = (uint32_t)strtoul(e, nullptr, 16); g_watch_page = g_watch & ~0xFFFu;
        mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ | PROT_EXEC);
        fprintf(stderr, "[watch] watching writes to %#x (page %#x read-only)\n", g_watch, g_watch_page);
        std::thread([] {            // the guest maps and protects memory itself (heap commit, MapMemory): the watched page is made read-only again whenever one of those has reopened it
            for (;;) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                std::lock_guard<std::mutex> lk(g_watch_mu);
                mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ | PROT_EXEC);
            }
        }).detach();
    });
}

cpu_t *cpu32_create(const cpu_ops_t *ops, void *user, unsigned) {
    arm_watch();
    auto *c = new cpu32;
    c->ops = *ops;
    c->user = user;
    return reinterpret_cast<cpu_t *>(c);
}

void cpu32_destroy(cpu_t *c) { delete C(c); }

int cpu32_run(cpu_t *cp) {
    cpu32 *c = C(cp);
    c->halt_req = false; c->faulted = false; c->svc_pending = false;
    for (;;) {
        jitenv *e = acquire(c);
        load(e, c->s);
        if (c->halt_req) e->jit->HaltExecution(HaltReason::UserDefined1);   // cpu_halt landed while no Jit was held
        HaltReason r = e->jit->Run();
        store(e, c->s);
        release(c, e);
        if (c->watch_hit) {
            c->watch_hit = false;
            fprintf(stderr, "[watch] write of %#x to %#x: pc=%#x lr=%#x sp=%#x r0=%#x r1=%#x r2=%#x r3=%#x r4=%#x\n", c->watch_value, g_watch, c->s.r[15], c->s.r[14], c->s.r[13], c->s.r[0], c->s.r[1], c->s.r[2], c->s.r[3], c->s.r[4]);
            fprintf(stderr, "[watch]   stack:");
            for (int i = 0; i < 200; i++) { uint32_t v = *(uint32_t *)(uintptr_t)(c->s.r[13] + 4 * i); if (v >= 0x200000 && v < 0x2400000) fprintf(stderr, " %#x", v); }
            fprintf(stderr, "\n");
            continue;                                     // run the store again (silently): the write itself was done by the callback
        }
        if (c->faulted) { dump_pc_log(); fprintf(stderr, "[RED] guest registers at the fault: pc=%#x lr=%#x sp=%#x r0=%#x r1=%#x r2=%#x r3=%#x r4=%#x r5=%#x r6=%#x r7=%#x r8=%#x r9=%#x r10=%#x r11=%#x\n", c->s.r[15], c->s.r[14], c->s.r[13], c->s.r[0], c->s.r[1], c->s.r[2], c->s.r[3], c->s.r[4], c->s.r[5], c->s.r[6], c->s.r[7], c->s.r[8], c->s.r[9], c->s.r[10], c->s.r[11]); if (getenv("NX_TRACE_SVC") || getenv("NX_FAULT_STACK")) { fprintf(stderr, "[RED] stack words that look like return addresses:"); for (int i = 0; i < 300; i++) { uint32_t v = *(uint32_t *)(uintptr_t)(c->s.r[13] + 4 * i); if (v >= 0x200000 && v < 0x2400000) fprintf(stderr, " %#x", v); } fprintf(stderr, "\n"); }
            return CPU_HALT_FAULT; }
        if (c->svc_pending) {
            c->svc_pending = false;
            static const bool trace = getenv("NX_TRACE_SVC") != nullptr;      // every SVC with its guest registers, to see where a title is
            if (c->svc_num >= 0x1000 && getenv("NX_STACK_AT") && hle_stub_name && hle_stub_name(c->svc_num) && strstr(hle_stub_name(c->svc_num), getenv("NX_STACK_AT")) && c->s.r[1] == 0) {   // NX_STACK_AT=<name part>: the return addresses on the stack of each such call whose second argument is 0
                fprintf(stderr, "[stack] %s lr=%#x:", hle_stub_name(c->svc_num), c->s.r[14]);
                for (int i = 0; i < 120; i++) { uint32_t v = *(uint32_t *)(uintptr_t)(c->s.r[13] + 4 * i); if (v >= 0x200000 && v < 0x2400000) fprintf(stderr, " %#x", v); }
                fprintf(stderr, "\n");
                if (const char *dump = getenv("NX_STACK_DUMP")) {            // NX_STACK_DUMP=<hex addr>: 24 words of guest memory there, for the object the call is about
                    uint32_t a = (uint32_t)strtoul(dump, nullptr, 16);
                    fprintf(stderr, "[stack] words at %#x:", a);
                    for (int i = 0; i < 24; i++) fprintf(stderr, " %#x", *(uint32_t *)(uintptr_t)(a + 4 * i));
                    fprintf(stderr, "\n");
                }
            }
            if (c->svc_num == 0x1FFE) {            // a trap set by NX_TRAP (main.c): log the registers, emulate the one `mov rd, rm` it displaced, go on
                uint32_t pc = c->s.r[15] - 4, orig = trap_orig(pc);
                fprintf(stderr, "[trap] pc=%#x r0=%#x r1=%#x r2=%#x r3=%#x r4=%#x lr=%#x\n", pc, c->s.r[0], c->s.r[1], c->s.r[2], c->s.r[3], c->s.r[4], c->s.r[14]);
                for (int k = 0; k < 9; k++) { uint32_t a = c->s.r[k]; if (a >= 0x200000 && a < 0xF0000000u) { const char *t = (const char *)(uintptr_t)a; int n = 0; while (n < 64 && t[n] >= 0x20 && t[n] < 0x7f) n++; if (n >= 4) fprintf(stderr, "[trap]   r%d -> \"%.*s\"\n", k, n, t); } }
                for (int k = 0; k < 11; k++) { uint32_t a = c->s.r[k]; if (a >= 0x1000000 && a < 0xF0000000u) { fprintf(stderr, "[trap]   r%d=%#x:", k, a); for (int j = 0; j < 24; j++) fprintf(stderr, " %#x", *(uint32_t *)(uintptr_t)(a + 4 * j)); fprintf(stderr, "\n"); } }
                if ((orig & 0xFFF00000) == 0xE3A00000 && !(orig & 0xF00)) c->s.r[(orig >> 12) & 15] = orig & 0xFF;
                if ((orig & 0xFFF00FF0) == 0xE1A00000) c->s.r[(orig >> 12) & 15] = c->s.r[orig & 15];
                continue;
            }
            if (g_svc_stats) {            // NX_SVC_STATS=1: what the guest asks of the host, tallied (svc or hook, caller) and printed every 2 s
                std::lock_guard<std::mutex> lk(g_stats_mu);
                g_stats[((uint64_t)(c->svc_num == 0xF0 ? c->s.r[12] : c->svc_num) << 32) | c->s.r[14]]++;
            }
            if (trace && c->svc_num >= 0x1000) fprintf(stderr, "[hook] %s(%#x, %#x, %#x, %#x) lr=%#x [r4-r8 %#x %#x %#x %#x %#x]\n", hle_stub_name && hle_stub_name(c->svc_num) ? hle_stub_name(c->svc_num) : "?", c->s.r[0], c->s.r[1], c->s.r[2], c->s.r[3], c->s.r[14], c->s.r[4], c->s.r[5], c->s.r[6], c->s.r[7], c->s.r[8]);
            if (trace) fprintf(stderr, "[svc32] %#x pc=%#x lr=%#x r0=%#x r1=%#x r2=%#x r3=%#x sp=%#x\n", c->svc_num, c->s.r[15], c->s.r[14], c->s.r[0], c->s.r[1], c->s.r[2], c->s.r[3], c->s.r[13]);
            if (trace && c->svc_num == 0x26) {            // Break: the stack words that look like return addresses say who aborted
                fprintf(stderr, "[svc32] Break stack:");
                for (int i = 0; i < 400; i++) { uint32_t v = *(uint32_t *)(uintptr_t)(c->s.r[13] + 4 * i); if (v >= 0x200000 && v < 0x2400000) fprintf(stderr, " %#x", v); }
                fprintf(stderr, "\n");
            }
            if (c->svc_num == 0x7FE && c->nested) { c->halt_req = true; return CPU_HALT_REQUESTED; }   // the guest function of cpu32_call returned
            if (c->svc_num == 0xF0) c->svc_num = c->s.r[12];      // a hooked Thumb function: movw r12, #n; svc #0xF0
            bool hook = c->svc_num >= 0x1000;                     // an HLE hook or stub: the host function stands in for the whole guest function, so return to the caller
            svc32_enter2(c->svc_num, c->s.r, c->s.ext, c->vx, c->fv, &c->svc_cx);
            c->in_svc = true;
            c->ops.svc(cp, c->svc_num, c->user);   // outside Run(): may block, may call cpu_halt
            c->in_svc = false;
            svc32_leave2(c->svc_num, c->vx, c->fret, c->s.r, c->s.ext, &c->svc_cx);
            if (trace && c->svc_num < 0x1000 && c->svc_num != 0x6 && c->s.r[0] != 0) fprintf(stderr, "[svc32]   → svc %#x returned %#x\n", c->svc_num, c->s.r[0]);
            if (hook) { uint32_t lr = c->s.r[14]; c->s.r[15] = lr & ~1u; c->s.cpsr = (lr & 1) ? c->s.cpsr | CPSR_T : c->s.cpsr & ~CPSR_T; }
            if (c->halt_req) return CPU_HALT_REQUESTED;
            continue;
        }
        if (c->halt_req || (static_cast<uint32_t>(r) & static_cast<uint32_t>(HaltReason::UserDefined1))) return CPU_HALT_REQUESTED;
    }
}

void cpu32_halt(cpu_t *cp) {
    cpu32 *c = C(cp);
    std::lock_guard<std::mutex> lk(g_mu);
    c->halt_req = true;
    if (jitenv *e = c->running.load()) e->jit->HaltExecution(HaltReason::UserDefined1);
}

uint64_t cpu32_get_x(cpu_t *cp, int i) {
    cpu32 *c = C(cp);
    if (c->in_svc && i >= 0 && i < 8) return c->vx[i];
    i = regidx(i);
    return i >= 0 && i < 16 ? c->s.r[i] : 0;
}
void cpu32_set_x(cpu_t *cp, int i, uint64_t v) {
    cpu32 *c = C(cp);
    if (c->in_svc && i >= 0 && i < 8) { c->vx[i] = v; return; }
    i = regidx(i);
    if (i >= 0 && i < 16) c->s.r[i] = (uint32_t)v;
}
uint64_t cpu32_get_pc(cpu_t *c) { return C(c)->s.r[15]; }
void cpu32_set_pc(cpu_t *c, uint64_t v) {          // an odd address is a Thumb entry, as for bx
    C(c)->s.r[15] = (uint32_t)v & ~1u;
    C(c)->s.cpsr = (v & 1) ? C(c)->s.cpsr | CPSR_T : C(c)->s.cpsr & ~CPSR_T;
}
uint64_t cpu32_get_sp(cpu_t *c) { return C(c)->s.r[13]; }
void cpu32_set_sp(cpu_t *c, uint64_t v) { C(c)->s.r[13] = (uint32_t)v; }
void cpu32_set_tls(cpu_t *c, uint64_t rw, uint64_t ro) { C(c)->s.tpidrurw = (uint32_t)rw; C(c)->s.tpidruro = (uint32_t)ro; }
void cpu32_get_v(cpu_t *c, int i, uint64_t out[2]) {
    if (C(c)->in_svc && i >= 0 && i < 8) { out[0] = C(c)->fv[i]; out[1] = 0; return; }
    const uint32_t *e = C(c)->s.ext + 4 * (i & 15);
    out[0] = e[0] | (uint64_t)e[1] << 32; out[1] = e[2] | (uint64_t)e[3] << 32;
}
void cpu32_set_v(cpu_t *c, int i, const uint64_t in[2]) {
    if (C(c)->in_svc && i == 0) { C(c)->fret = in[0]; return; }
    if (C(c)->in_svc) return;
    uint32_t *e = C(c)->s.ext + 4 * (i & 15);
    e[0] = (uint32_t)in[0]; e[1] = (uint32_t)(in[0] >> 32); e[2] = (uint32_t)in[1]; e[3] = (uint32_t)(in[1] >> 32);
}
void cpu32_invalidate(cpu_t *, uint64_t addr, uint64_t len) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &e : g_envs) e->jit->InvalidateCacheRange((uint32_t)addr, (size_t)len);
}
