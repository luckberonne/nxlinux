// Guest CPUs on a small pool of dynarmic Jits. A guest thread owns a saved register context (cpu_t) and borrows a Jit only
// while it runs guest code: an svc leaves Run(), the kernel handles it outside (where it may block without holding a Jit),
// and the thread resumes on whichever Jit is free. Translated code is therefore shared by all guest threads, as on a real
// console with 3-4 cores, and creating a guest thread costs nothing (a Jit takes ~18 ms to create).
#include "cpu.h"
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

using namespace Dynarmic;
using namespace Dynarmic::A64;

struct ctx {
    uint64_t x[31] = {};
    uint64_t sp = 0, pc = 0;
    Vector v[32] = {};
    uint32_t fpcr = 0, fpsr = 0, pstate = 0;
    uint64_t tpidr = 0, tpidrro = 0;
};

struct jitenv;

struct cpu {
    cpu_ops_t ops;
    void *user;
    ctx s;
    bool svc_pending = false, halt_req = false, faulted = false;
    uint32_t svc_num = 0;
    std::atomic<jitenv *> running{nullptr};   // the Jit this cpu is on right now, null while outside Run()
};

struct jitenv final : UserCallbacks {
    std::unique_ptr<Jit> jit;
    unsigned id = 0;
    cpu *cur = nullptr;
    uint64_t tpidr = 0, tpidrro = 0;           // the Jit's TLS registers while a cpu is loaded

    template <class T> static T rd(uint64_t a) { T v; memcpy(&v, (const void *)a, sizeof v); return v; }
    template <class T> static void wr(uint64_t a, T v) { memcpy((void *)a, &v, sizeof v); }
    template <class T> static bool excl(uint64_t a, T v, T expected) {
        return __atomic_compare_exchange_n((T *)a, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    uint8_t MemoryRead8(VAddr a) override { return rd<uint8_t>(a); }
    uint16_t MemoryRead16(VAddr a) override { return rd<uint16_t>(a); }
    uint32_t MemoryRead32(VAddr a) override { return rd<uint32_t>(a); }
    uint64_t MemoryRead64(VAddr a) override { return rd<uint64_t>(a); }
    Vector MemoryRead128(VAddr a) override { return rd<Vector>(a); }
    void MemoryWrite8(VAddr a, uint8_t v) override { wr(a, v); }
    void MemoryWrite16(VAddr a, uint16_t v) override { wr(a, v); }
    void MemoryWrite32(VAddr a, uint32_t v) override { wr(a, v); }
    void MemoryWrite64(VAddr a, uint64_t v) override { wr(a, v); }
    void MemoryWrite128(VAddr a, Vector v) override { wr(a, v); }
    bool MemoryWriteExclusive8(VAddr a, uint8_t v, uint8_t e) override { return excl(a, v, e); }
    bool MemoryWriteExclusive16(VAddr a, uint16_t v, uint16_t e) override { return excl(a, v, e); }
    bool MemoryWriteExclusive32(VAddr a, uint32_t v, uint32_t e) override { return excl(a, v, e); }
    bool MemoryWriteExclusive64(VAddr a, uint64_t v, uint64_t e) override { return excl(a, v, e); }
    bool MemoryWriteExclusive128(VAddr a, Vector v, Vector e) override {
        unsigned __int128 vv, ee; memcpy(&vv, &v, 16); memcpy(&ee, &e, 16);
        return excl(a, vv, ee);
    }
    void InterpreterFallback(VAddr pc, size_t) override { fault(pc, 1); }
    void ExceptionRaised(VAddr pc, Exception) override { fault(pc, 2); }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1ull << 40; }
    uint64_t GetCNTPCT() override { return cur->ops.cntpct(); }
    void CallSVC(uint32_t swi) override {      // leave Run(); the kernel handles the svc outside, see cpu_run
        cur->svc_pending = true; cur->svc_num = swi;
        jit->HaltExecution(HaltReason::UserDefined2);
    }
    void fault(uint64_t pc, int kind) {
        cur->faulted = true;
        if (cur->ops.fault) cur->ops.fault(reinterpret_cast<cpu_t *>(cur), pc, kind, cur->user);
        jit->HaltExecution(HaltReason::UserDefined3);
    }
};

static ExclusiveMonitor &monitor() {
    static ExclusiveMonitor m(CPU_MAX_CORES);
    return m;
}

// ---- the pool
static std::mutex g_mu;
static std::condition_variable g_cv;
static std::vector<std::unique_ptr<jitenv>> g_envs;   // every Jit created
static std::vector<jitenv *> g_free;
static void (*g_jit_hook)(void);
void cpu_set_jit_created_hook(void (*fn)(void)) { g_jit_hook = fn; }
static unsigned g_max_jits, g_created;     // g_created: ids handed out, including Jits still being built

static unsigned max_jits() {
    if (!g_max_jits) {
        const char *e = getenv("NX_JITS");
        long n = e ? atol(e) : 8;
        g_max_jits = n < 1 ? 1 : n > CPU_MAX_CORES ? CPU_MAX_CORES : (unsigned)n;
    }
    return g_max_jits;
}

static std::unique_ptr<jitenv> make_env(unsigned id) {
    auto e = std::make_unique<jitenv>();
    e->id = id;
    UserConfig cfg{};
    cfg.callbacks = e.get();
    cfg.fastmem_pointer = 0;                    // guest address == host address
    cfg.fastmem_address_space_bits = 48;
    cfg.silently_mirror_fastmem = false;
    cfg.fastmem_exclusive_access = false;       // exclusives go through MemoryWriteExclusive* (compare-exchange)
    cfg.tpidr_el0 = &e->tpidr;
    cfg.tpidrro_el0 = &e->tpidrro;
    cfg.wall_clock_cntpct = true;
    cfg.enable_cycle_counting = false;
    cfg.global_monitor = &monitor();
    cfg.processor_id = id;
    e->jit = std::make_unique<Jit>(cfg);
    return e;
}

static jitenv *acquire(cpu *c) {
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        if (!g_free.empty()) { jitenv *e = g_free.back(); g_free.pop_back(); e->cur = c; c->running = e; return e; }
        if (g_created < max_jits()) {
            unsigned id = g_created++;          // reserved under the lock: ids are the exclusive-monitor slots and must be unique
            lk.unlock();                        // ~18 ms: do not hold the pool while building
            auto e = make_env(id);
            if (g_jit_hook) g_jit_hook();
            lk.lock();
            jitenv *p = e.get();
            g_envs.push_back(std::move(e));
            p->cur = c; c->running = p;
            return p;
        }
        g_cv.wait(lk);
    }
}

static void release(cpu *c, jitenv *e) {
    std::lock_guard<std::mutex> lk(g_mu);
    c->running = nullptr;
    e->jit->ClearHalt(HaltReason::UserDefined1);
    e->jit->ClearHalt(HaltReason::UserDefined2);
    e->jit->ClearHalt(HaltReason::UserDefined3);
    e->cur = nullptr;
    g_free.push_back(e);
    g_cv.notify_one();
}

static cpu *C(cpu_t *c) { return reinterpret_cast<cpu *>(c); }

cpu_t *cpu_create(const cpu_ops_t *ops, void *user, unsigned core) {
    (void)core;                                  // the pool decides which Jit runs a thread
    auto *c = new cpu;
    c->ops = *ops;
    c->user = user;
    return reinterpret_cast<cpu_t *>(c);
}

void cpu_destroy(cpu_t *c) { delete C(c); }

static void load(jitenv *e, const ctx &s) {
    Jit &j = *e->jit;
    std::array<uint64_t, 31> x;
    memcpy(x.data(), s.x, sizeof s.x);
    j.SetRegisters(x);
    j.SetSP(s.sp); j.SetPC(s.pc);
    for (int i = 0; i < 32; i++) j.SetVector(i, s.v[i]);
    j.SetFpcr(s.fpcr); j.SetFpsr(s.fpsr); j.SetPstate(s.pstate);
    e->tpidr = s.tpidr; e->tpidrro = s.tpidrro;
}
static void store(jitenv *e, ctx &s) {
    Jit &j = *e->jit;
    auto x = j.GetRegisters();
    memcpy(s.x, x.data(), sizeof s.x);
    s.sp = j.GetSP(); s.pc = j.GetPC();
    for (int i = 0; i < 32; i++) s.v[i] = j.GetVector(i);
    s.fpcr = j.GetFpcr(); s.fpsr = j.GetFpsr(); s.pstate = j.GetPstate();
    s.tpidr = e->tpidr; s.tpidrro = e->tpidrro;
}

int cpu_run(cpu_t *cp) {
    cpu *c = C(cp);
    c->halt_req = false; c->faulted = false; c->svc_pending = false;
    for (;;) {
        jitenv *e = acquire(c);
        load(e, c->s);
        if (c->halt_req) e->jit->HaltExecution(HaltReason::UserDefined1);   // cpu_halt landed while no Jit was held
        HaltReason r = e->jit->Run();
        store(e, c->s);
        release(c, e);
        if (c->faulted) return CPU_HALT_FAULT;
        if (c->svc_pending) {
            c->svc_pending = false;
            c->ops.svc(cp, c->svc_num, c->user);   // outside Run(): may block, may call cpu_halt
            if (c->halt_req) return CPU_HALT_REQUESTED;
            continue;
        }
        if (c->halt_req || (static_cast<uint32_t>(r) & static_cast<uint32_t>(HaltReason::UserDefined1))) return CPU_HALT_REQUESTED;
        // CacheInvalidation or similar: run again
    }
}

void cpu_halt(cpu_t *cp) {
    cpu *c = C(cp);
    std::lock_guard<std::mutex> lk(g_mu);
    c->halt_req = true;
    if (jitenv *e = c->running.load()) e->jit->HaltExecution(HaltReason::UserDefined1);
}

uint64_t cpu_get_x(cpu_t *c, int i) { return i == 31 ? 0 : C(c)->s.x[i]; }
void cpu_set_x(cpu_t *c, int i, uint64_t v) { if (i < 31) C(c)->s.x[i] = v; }
uint64_t cpu_get_pc(cpu_t *c) { return C(c)->s.pc; }
void cpu_set_pc(cpu_t *c, uint64_t v) { C(c)->s.pc = v; }
uint64_t cpu_get_sp(cpu_t *c) { return C(c)->s.sp; }
void cpu_set_sp(cpu_t *c, uint64_t v) { C(c)->s.sp = v; }
void cpu_set_tls(cpu_t *c, uint64_t a, uint64_t b) { C(c)->s.tpidr = a; C(c)->s.tpidrro = b; }
void cpu_get_v(cpu_t *c, int i, uint64_t out[2]) { out[0] = C(c)->s.v[i][0]; out[1] = C(c)->s.v[i][1]; }
void cpu_set_v(cpu_t *c, int i, const uint64_t in[2]) { C(c)->s.v[i] = {in[0], in[1]}; }
void cpu_invalidate(cpu_t *, uint64_t addr, uint64_t len) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &e : g_envs) e->jit->InvalidateCacheRange(addr, len);
}
