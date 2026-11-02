// Guest CPUs on a small pool of dynarmic Jits. A guest thread owns a saved register context (cpu_t) and borrows a Jit only
// while it runs guest code: an svc leaves Run(), the kernel handles it outside (where it may block without holding a Jit),
// and the thread resumes on whichever Jit is free. Translated code is therefore shared by all guest threads, as on a real
// console with 3-4 cores, and creating a guest thread costs nothing (a Jit takes ~18 ms to create).
#include "cpu.h"
extern "C" { uint64_t guest_mem_base(void) __attribute__((weak)); uint64_t guest_mem_end(void) __attribute__((weak)); }       // absent in unit tests that bring their own memory
#include "cpu32.h"
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <mutex>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/mman.h>

using namespace Dynarmic;
using namespace Dynarmic::A64;

// ---- debugging aids for a 64-bit title (the cpu32.cpp ones): NX_WATCH=<hex address> stops at every write to that 4-byte word and prints who wrote it; NX_TRACE_PC=<file> logs every
// instruction the Jit translates (see cpu32.cpp)
static uint64_t g_watch, g_watch_page; static std::mutex g_watch_mu;
static std::vector<uint64_t> g_pc_log; static std::mutex g_pc_mu; static bool g_pc_trace;
static void dump_pc_log() {
    const char *f = getenv("NX_TRACE_PC");
    if (!f || !g_pc_trace) return;
    std::lock_guard<std::mutex> lk(g_pc_mu);
    if (FILE *o = fopen(f, "wb")) { fwrite(g_pc_log.data(), 8, g_pc_log.size(), o); fclose(o); fprintf(stderr, "[trace] %zu translated instructions written to %s (u64 addresses)\n", g_pc_log.size(), f); }
}
static void arm_aids() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (getenv("NX_TRACE_PC")) {
            g_pc_trace = true; atexit(dump_pc_log);
            if (const char *after = getenv("NX_TRACE_PC_AFTER")) { int secs = atoi(after); std::thread([secs] { std::this_thread::sleep_for(std::chrono::seconds(secs)); dump_pc_log(); }).detach(); }
        }
        if (const char *e = getenv("NX_WATCH")) {
            g_watch = strtoull(e, nullptr, 16); g_watch_page = g_watch & ~0xFFFull;
            mprotect((void *)g_watch_page, 0x1000, PROT_READ | PROT_EXEC);
            fprintf(stderr, "[watch] watching writes to %#llx (page %#llx read-only)\n", (unsigned long long)g_watch, (unsigned long long)g_watch_page);
            std::thread([] { for (;;) { std::this_thread::sleep_for(std::chrono::microseconds(200)); std::lock_guard<std::mutex> lk(g_watch_mu); mprotect((void *)g_watch_page, 0x1000, PROT_READ | PROT_EXEC); } }).detach();
        }
    });
}

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
    int nested = 0;                           // cpu_call64 depth
    bool watch_hit = false, watch_skip = false; uint64_t watch_value = 0;       // see NX_WATCH
    uint64_t bad_addr = 0;                    // the address of the access that stopped this thread
    std::atomic<jitenv *> running{nullptr};   // the Jit this cpu is on right now, null while outside Run()
};

struct jitenv;
static void store(jitenv *e, ctx &s);     // defined below: a fault reads the registers of the Jit before it reports them

struct jitenv final : UserCallbacks {
    std::unique_ptr<Jit> jit;
    unsigned id = 0;
    cpu *cur = nullptr;
    uint64_t tpidr = 0, tpidrro = 0;           // the Jit's TLS registers while a cpu is loaded
    std::atomic<uint64_t> last_code{0}, ncode{0};   // the last instruction the translator asked for, and how many it asked for in all (the thread dump shows a translation that never ends)

    // An access outside the guest window is a guest bug (a NULL pointer, a stale address): it stops this thread, with the registers, instead of faulting in the host
    bool ok(uint64_t a, size_t n) {
        if (!guest_mem_base || !guest_mem_end || (a >= guest_mem_base() && a < guest_mem_end() && n <= guest_mem_end() - a)) return true;
        cur->bad_addr = a; cur->faulted = true;
        jit->HaltExecution(HaltReason::MemoryAbort);
        return false;
    }
    template <class T> T rd(uint64_t a) { T v{}; if (ok(a, sizeof v)) memcpy(&v, (const void *)a, sizeof v); return v; }
    void watched(uint64_t a, size_t n, uint64_t v) {
        if (a < g_watch + 4 && a + n > g_watch) {
            if (cur->watch_skip) cur->watch_skip = false;
            else { cur->watch_hit = true; cur->watch_skip = true; cur->watch_value = v; jit->HaltExecution(HaltReason::MemoryAbort); }
        }
    }
    template <class T> void wr(uint64_t a, T v) {
        if (!ok(a, sizeof v)) return;
        if (g_watch_page && (a & ~0xFFFull) == g_watch_page) {
            std::lock_guard<std::mutex> lk(g_watch_mu);
            mprotect((void *)g_watch_page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
            memcpy((void *)a, &v, sizeof v);
            mprotect((void *)g_watch_page, 0x1000, PROT_READ | PROT_EXEC);
            if constexpr (sizeof(T) <= 8) watched(a, sizeof v, (uint64_t)v); else { uint64_t w = 0; size_t o = g_watch > a ? (size_t)(g_watch - a) : 0; if (o < sizeof v) memcpy(&w, (const char *)&v + o, std::min<size_t>(8, sizeof v - o)); watched(a, sizeof v, w); }
            return;
        }
        memcpy((void *)a, &v, sizeof v);
    }
    template <class T> bool excl(uint64_t a, T v, T expected) {
        if (!ok(a, sizeof v)) return false;
        if (g_watch_page && (a & ~0xFFFull) == g_watch_page) {
            std::lock_guard<std::mutex> lk(g_watch_mu);
            mprotect((void *)g_watch_page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
            bool done = __atomic_compare_exchange_n((T *)a, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            mprotect((void *)g_watch_page, 0x1000, PROT_READ | PROT_EXEC);
            if (done) { if constexpr (sizeof(T) <= 8) watched(a, sizeof v, (uint64_t)v); else { uint64_t w = 0; size_t o = g_watch > a ? (size_t)(g_watch - a) : 0; if (o < sizeof v) memcpy(&w, (const char *)&v + o, std::min<size_t>(8, sizeof v - o)); watched(a, sizeof v, w); } }
            return done;
        }
        return __atomic_compare_exchange_n((T *)a, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    std::optional<std::uint32_t> MemoryReadCode(VAddr a) override {
        last_code.store(a, std::memory_order_relaxed); ncode.fetch_add(1, std::memory_order_relaxed);
        if (guest_mem_base && guest_mem_end && !(a >= guest_mem_base() && a < guest_mem_end() && 4 <= guest_mem_end() - a)) {      // code outside the guest window (a jump through a wild pointer): dynarmic then raises a fault for the block instead of merging zero words for ever
            cur->bad_addr = a; cur->faulted = true; return std::nullopt;
        }
        if (g_pc_trace) { std::lock_guard<std::mutex> lk(g_pc_mu); g_pc_log.push_back(a); }
        return MemoryRead32(a);
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
        store(this, cur->s); cur->s.pc = pc;      // the registers at the fault (the copy in cpu is from the last stop, an SVC)
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
    if (!getenv("NX_SLOWMEM")) cfg.fastmem_pointer = 0;     // guest address == host address (NX_SLOWMEM=1: no fast memory, every access goes through the callbacks)
    if (getenv("NX_JIT_NOOPT")) cfg.optimizations = no_optimizations;        // NX_JIT_NOOPT=1: dynarmic's IR and dispatch optimizations off, to tell a translation bug from the title's own
    cfg.fastmem_address_space_bits = 48;
    cfg.silently_mirror_fastmem = false;
    cfg.fastmem_exclusive_access = false;       // exclusives go through MemoryWriteExclusive* (compare-exchange). Inline ones (true) were measured in the race: no faster (19.7-20.5 ms against 18.7-20.6), and dynarmic warns that they do not reproduce ABA
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
    e->jit->ClearHalt(HaltReason::UserDefined4);
    e->jit->ClearHalt(HaltReason::UserDefined5);
    e->cur = nullptr;
    g_free.push_back(e);
    g_cv.notify_one();
}

extern "C" __attribute__((weak)) void nx_prof_guest(uint64_t pc, uint64_t lr);          // the profiler (prof.c) is not linked into every test
extern "C" __attribute__((weak)) int nx_prof_guest_on(void);
extern "C" void cpu_prof_poke(void) {                  // the profiler's thread: every Jit that runs a guest thread stops at the end of its block, and cpu_run notes the program counter
    if (!nx_prof_guest_on || !nx_prof_guest_on()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &e : g_envs) if (e->cur) e->jit->HaltExecution(HaltReason::UserDefined4);
}

static cpu *C(cpu_t *c) { return reinterpret_cast<cpu *>(c); }

// Preemption: the kernel's scheduler only gives a core to another thread at an SVC, so a guest thread that spins on memory without ever calling the kernel (a wait for a thread of its own
// core) would keep the core for ever. cpu_preempt stops that thread's Jit at the end of its current block; cpu_run then calls the hook (which may block, the Jit is back in the pool by
// then) and runs on. A stop that lands on a thread which just left that Jit is harmless: the hook looks at the situation again and the thread continues.
static void (*g_preempt_hook)(cpu_t *);
extern "C" void cpu_set_preempt_hook(void (*fn)(cpu_t *)) { g_preempt_hook = fn; }
extern "C" int cpu_translating(cpu_t *cp, uint64_t *pc, uint64_t *count) {      // the Jit this cpu is on: the last address its translator read and the reads so far (0 if the cpu is on no Jit)
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &e : g_envs) if (e->cur == C(cp)) { *pc = e->last_code.load(); *count = e->ncode.load(); return 1; }
    return 0;
}
extern "C" void cpu_preempt(cpu_t *cp) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &e : g_envs) if (e->cur == C(cp)) e->jit->HaltExecution(HaltReason::UserDefined5);
}

static bool g_a32;
static uint64_t g_return_pad64;
void cpu_set_return_pad64(uint64_t addr) { g_return_pad64 = addr; }
uint64_t cpu_call64(cpu_t *cp, uint64_t fn, const uint64_t *args, int nargs) {
    if (g_a32 || !g_return_pad64) return 0;
    cpu *c = C(cp);
    ctx saved = c->s; bool halt = c->halt_req;
    for (int i = 0; i < nargs && i < 8; i++) c->s.x[i] = args[i];
    c->s.sp = (saved.sp - 0x200) & ~0xFull;      // a frame below the caller's
    c->s.x[30] = g_return_pad64; c->s.pc = fn;
    c->nested++;
    cpu_run(cp);
    c->nested--;
    uint64_t ret = c->s.x[0];
    c->s = saved; c->halt_req = halt;
    return ret;
}
uint32_t cpu_call32(cpu_t *c, uint32_t fn, const uint32_t *args, int nargs) { return g_a32 ? cpu32_call(c, fn, args, nargs) : 0; }
void cpu_set_aarch32(int on) { g_a32 = on != 0; }
int cpu_is_aarch32(void) { return g_a32; }

cpu_t *cpu_create(const cpu_ops_t *ops, void *user, unsigned core) {
    if (g_a32) return cpu32_create(ops, user, core);
    (void)core;                                  // the pool decides which Jit runs a thread
    auto *c = new cpu;
    c->ops = *ops;
    c->user = user;
    return reinterpret_cast<cpu_t *>(c);
}

void cpu_destroy(cpu_t *c) { if (g_a32) cpu32_destroy(c); else delete C(c); }

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

#include <map>
static std::map<uint64_t, uint32_t> g_trap64;            // address -> the instruction a trap replaced (NX_TRAP)
void cpu_set_trap64(uint64_t addr) { uint32_t *p = (uint32_t *)addr; g_trap64[addr] = *p; *p = 0xD4000001u | (0x1FFEu << 5); }       // svc #0x1FFE

int cpu_run(cpu_t *cp) {
    if (g_a32) return cpu32_run(cp);
    arm_aids();
    cpu *c = C(cp);
    c->halt_req = false; c->faulted = false; c->svc_pending = false;
    for (;;) {
        jitenv *e = acquire(c);
        load(e, c->s);
        if (c->halt_req) e->jit->HaltExecution(HaltReason::UserDefined1);   // cpu_halt landed while no Jit was held
        HaltReason r = e->jit->Run();
        store(e, c->s);
        release(c, e);
        if (static_cast<uint32_t>(r) & static_cast<uint32_t>(HaltReason::UserDefined4)) {      // a profiler sample, not a stop of the guest
            if (nx_prof_guest) nx_prof_guest(c->s.pc, c->s.x[30]);
            if (!c->watch_hit && !c->faulted && !c->svc_pending && !c->halt_req) continue;
        }
        if (static_cast<uint32_t>(r) & static_cast<uint32_t>(HaltReason::UserDefined5)) {      // the kernel's scheduler asks this thread to give its core up
            if (g_preempt_hook) g_preempt_hook(cp);
            if (!c->watch_hit && !c->faulted && !c->svc_pending && !c->halt_req) continue;
        }
        if (c->watch_hit) {
            c->watch_hit = false;
            fprintf(stderr, "[watch] write of %#llx to %#llx: pc=%#llx lr=%#llx sp=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x19=%#llx\n", (unsigned long long)c->watch_value, (unsigned long long)g_watch, (unsigned long long)c->s.pc,
                    (unsigned long long)c->s.x[30], (unsigned long long)c->s.sp, (unsigned long long)c->s.x[0], (unsigned long long)c->s.x[1], (unsigned long long)c->s.x[2], (unsigned long long)c->s.x[3], (unsigned long long)c->s.x[19]);
            fprintf(stderr, "[watch]   frames:"); uint64_t fp = c->s.x[29];
            for (int i = 0; i < 14 && guest_mem_base && fp >= guest_mem_base() && fp + 16 <= guest_mem_end() && !(fp & 7); i++) { fprintf(stderr, " %#llx", (unsigned long long)*(uint64_t *)(fp + 8)); fp = *(uint64_t *)fp; }
            fprintf(stderr, "\n");
            continue;                                      // run the store again (silently): the write itself was done by the callback
        }
        if (c->faulted) {
            if (c->bad_addr) {
                fprintf(stderr, "[RED] guest access to unmapped address %#llx at pc=%#llx sp=%#llx\n", (unsigned long long)c->bad_addr, (unsigned long long)c->s.pc, (unsigned long long)c->s.sp);
                for (int i = 0; i < 31; i += 4) fprintf(stderr, "[RED]   x%d=%#llx x%d=%#llx x%d=%#llx x%d=%#llx\n", i, (unsigned long long)c->s.x[i], i + 1, (unsigned long long)c->s.x[i + 1], i + 2, (unsigned long long)c->s.x[i + 2], i + 3, (unsigned long long)(i + 3 < 31 ? c->s.x[i + 3] : 0));
                { uint64_t fp = c->s.x[29]; fprintf(stderr, "[RED]   frames (x29 chain):");        // the frame-pointer chain: who called whom
                  for (int i = 0; i < 16 && fp >= guest_mem_base() && fp + 16 <= guest_mem_end() && !(fp & 7); i++) { fprintf(stderr, " %#llx", (unsigned long long)*(uint64_t *)(fp + 8)); fp = *(uint64_t *)fp; }
                  fprintf(stderr, "\n");
                  fp = c->s.x[29];                                                              // and the two words just below each frame pointer: usually the callee-saved x20, x19 (the `this` of that frame)
                  for (int i = 0; i < 8 && fp >= guest_mem_base() + 16 && fp + 16 <= guest_mem_end() && !(fp & 7); i++) { fprintf(stderr, "[RED]   frame %d fp=%#llx [fp-16]=%#llx [fp-8]=%#llx ret=%#llx\n", i, (unsigned long long)fp, (unsigned long long)*(uint64_t *)(fp - 16), (unsigned long long)*(uint64_t *)(fp - 8), (unsigned long long)*(uint64_t *)(fp + 8)); if (const char *fd = getenv("NX_FAULT_DUMP")) if (i == atoi(fd)) for (int k = 0; k < 2; k++) {            // NX_FAULT_DUMP=<frame>: 24 words at each of that frame's two saved registers
                      uint64_t a = *(uint64_t *)(fp - 16 + 8 * k); if (a < guest_mem_base() || a + 96 > guest_mem_end()) continue;
                      fprintf(stderr, "[RED]     *%#llx:", (unsigned long long)a); for (int j = 0; j < 12; j++) fprintf(stderr, " %#llx", (unsigned long long)*(uint64_t *)(a + 8 * j)); fprintf(stderr, "\n"); }
                    fp = *(uint64_t *)fp; } }
                if (c->ops.fault) c->ops.fault(cp, c->s.pc, 3, c->user);
            }
            return CPU_HALT_FAULT;
        }
        if (c->svc_pending) {
            c->svc_pending = false;
            if (c->svc_num == 0x7FE && c->nested) return CPU_HALT_REQUESTED;      // the guest function of cpu_call64 returned
            if (c->svc_num == 0x1FFE) {            // a trap set by NX_TRAP: log the registers and any strings they point at, emulate the one `mov xd, xm` it displaced, go on
                uint64_t pc = c->s.pc - 4; auto it = g_trap64.find(pc); uint32_t orig = it == g_trap64.end() ? 0 : it->second;
                fprintf(stderr, "[trap] pc=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x19=%#llx x20=%#llx x21=%#llx x22=%#llx x23=%#llx lr=%#llx\n", (unsigned long long)pc, (unsigned long long)c->s.x[0], (unsigned long long)c->s.x[1], (unsigned long long)c->s.x[2], (unsigned long long)c->s.x[3],
                        (unsigned long long)c->s.x[19], (unsigned long long)c->s.x[20], (unsigned long long)c->s.x[21], (unsigned long long)c->s.x[22], (unsigned long long)c->s.x[23], (unsigned long long)c->s.x[30]);
                for (int k = 0; k < 29; k++) { uint64_t a = c->s.x[k]; if (guest_mem_base && a >= guest_mem_base() && a + 64 < guest_mem_end()) { const char *t = (const char *)a; int n = 0; while (n < 80 && t[n] >= 0x20 && t[n] < 0x7f) n++; if (n >= 4) fprintf(stderr, "[trap]   x%d -> \"%.*s\"\n", k, n, t); } }
                if ((orig & 0xFFE0FFE0) == 0xAA0003E0) c->s.x[orig & 31] = c->s.x[(orig >> 16) & 31];
                c->s.pc = pc + 4; continue;
            }
            c->ops.svc(cp, c->svc_num, c->user);   // outside Run(): may block, may call cpu_halt
            if (c->halt_req) return CPU_HALT_REQUESTED;
            continue;
        }
        if (c->halt_req || (static_cast<uint32_t>(r) & static_cast<uint32_t>(HaltReason::UserDefined1))) return CPU_HALT_REQUESTED;
        // CacheInvalidation or similar: run again
    }
}

void cpu_halt(cpu_t *cp) {
    if (g_a32) return cpu32_halt(cp);
    cpu *c = C(cp);
    std::lock_guard<std::mutex> lk(g_mu);
    c->halt_req = true;
    if (jitenv *e = c->running.load()) e->jit->HaltExecution(HaltReason::UserDefined1);
}

uint64_t cpu_get_x(cpu_t *c, int i) { if (g_a32) return cpu32_get_x(c, i); return i == 31 ? 0 : C(c)->s.x[i]; }
void cpu_set_x(cpu_t *c, int i, uint64_t v) { if (g_a32) return cpu32_set_x(c, i, v); if (i < 31) C(c)->s.x[i] = v; }
uint64_t cpu_get_pc(cpu_t *c) { if (g_a32) return cpu32_get_pc(c); return C(c)->s.pc; }
void cpu_set_pc(cpu_t *c, uint64_t v) { if (g_a32) return cpu32_set_pc(c, v); C(c)->s.pc = v; }
uint64_t cpu_get_sp(cpu_t *c) { if (g_a32) return cpu32_get_sp(c); return C(c)->s.sp; }
void cpu_set_sp(cpu_t *c, uint64_t v) { if (g_a32) return cpu32_set_sp(c, v); C(c)->s.sp = v; }
void cpu_set_tls(cpu_t *c, uint64_t a, uint64_t b) { if (g_a32) return cpu32_set_tls(c, a, b); C(c)->s.tpidr = a; C(c)->s.tpidrro = b; }
void cpu_get_v(cpu_t *c, int i, uint64_t out[2]) { if (g_a32) return cpu32_get_v(c, i, out); out[0] = C(c)->s.v[i][0]; out[1] = C(c)->s.v[i][1]; }
void cpu_set_v(cpu_t *c, int i, const uint64_t in[2]) { if (g_a32) return cpu32_set_v(c, i, in); C(c)->s.v[i] = {in[0], in[1]}; }
void cpu_invalidate(cpu_t *c, uint64_t addr, uint64_t len) {
    if (g_a32) return cpu32_invalidate(c, addr, len);
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &e : g_envs) e->jit->InvalidateCacheRange(addr, len);
}
