#include "cpu.h"
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace Dynarmic;
using namespace Dynarmic::A64;

static ExclusiveMonitor &monitor() {
    static ExclusiveMonitor m(CPU_MAX_CORES);
    return m;
}

struct cpu final : UserCallbacks {
    cpu_ops_t ops;
    void *user;
    std::unique_ptr<Jit> jit;
    uint64_t tpidr = 0, tpidrro = 0;
    int halt = CPU_HALT_NONE;

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
    uint64_t GetCNTPCT() override { return ops.cntpct(); }
    void CallSVC(uint32_t swi) override { ops.svc(reinterpret_cast<cpu_t *>(this), swi, user); }

    void fault(uint64_t pc, int kind) {
        halt = CPU_HALT_FAULT;
        if (ops.fault) ops.fault(reinterpret_cast<cpu_t *>(this), pc, kind, user);
        jit->HaltExecution();
    }
};

static cpu *C(cpu_t *c) { return reinterpret_cast<cpu *>(c); }

cpu_t *cpu_create(const cpu_ops_t *ops, void *user, unsigned core) {
    auto *c = new cpu;
    c->ops = *ops;
    c->user = user;
    UserConfig cfg{};
    cfg.callbacks = c;
    cfg.fastmem_pointer = 0;                       // guest address == host address
    cfg.fastmem_address_space_bits = 48;
    cfg.silently_mirror_fastmem = false;
    cfg.fastmem_exclusive_access = false;          // exclusives go through MemoryWriteExclusive* (compare-exchange)
    cfg.tpidr_el0 = &c->tpidr;
    cfg.tpidrro_el0 = &c->tpidrro;
    cfg.wall_clock_cntpct = true;
    cfg.enable_cycle_counting = false;
    cfg.global_monitor = &monitor();
    cfg.processor_id = core;
    c->jit = std::make_unique<Jit>(cfg);
    return reinterpret_cast<cpu_t *>(c);
}

void cpu_destroy(cpu_t *c) { delete C(c); }

int cpu_run(cpu_t *c) {
    cpu *p = C(c);
    p->halt = CPU_HALT_NONE;
    p->jit->ClearHalt();
    HaltReason r = p->jit->Run();
    if (p->halt == CPU_HALT_FAULT) return CPU_HALT_FAULT;
    return (static_cast<uint32_t>(r) & static_cast<uint32_t>(HaltReason::UserDefined1)) ? CPU_HALT_REQUESTED : CPU_HALT_NONE;
}

void cpu_halt(cpu_t *c) { C(c)->jit->HaltExecution(); }
uint64_t cpu_get_x(cpu_t *c, int i) { return C(c)->jit->GetRegister(i); }
void cpu_set_x(cpu_t *c, int i, uint64_t v) { C(c)->jit->SetRegister(i, v); }
uint64_t cpu_get_pc(cpu_t *c) { return C(c)->jit->GetPC(); }
void cpu_set_pc(cpu_t *c, uint64_t v) { C(c)->jit->SetPC(v); }
uint64_t cpu_get_sp(cpu_t *c) { return C(c)->jit->GetSP(); }
void cpu_set_sp(cpu_t *c, uint64_t v) { C(c)->jit->SetSP(v); }
void cpu_set_tls(cpu_t *c, uint64_t a, uint64_t b) { C(c)->tpidr = a; C(c)->tpidrro = b; }
void cpu_get_v(cpu_t *c, int i, uint64_t out[2]) { auto v = C(c)->jit->GetVector(i); out[0] = v[0]; out[1] = v[1]; }
void cpu_set_v(cpu_t *c, int i, const uint64_t in[2]) { C(c)->jit->SetVector(i, {in[0], in[1]}); }
void cpu_invalidate(cpu_t *c, uint64_t addr, uint64_t len) { C(c)->jit->InvalidateCacheRange(addr, len); }
