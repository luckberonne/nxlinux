// Spike: dynarmic A64 with an identity-mapped fastmem arena (guest address == host address),
// an SVC that calls a host C function through the AAPCS64 argument registers, and TLS registers.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace Dynarmic::A64;

// Guest window, reserved at a fixed address so guest pointers are host pointers.
// 0x20'0000'0000 = 128 GB, inside the 39-bit range.
static constexpr uint64_t kBase = 0x2000000000ull;
static constexpr size_t kSize = 64ull << 20;

static int g_hle_calls;
extern "C" uint64_t hle_add(uint64_t a, uint64_t b, uint64_t c) { g_hle_calls++; return a + b + c; }
extern "C" void hle_poke(uint32_t* out, const char* name) { *out = (uint32_t)strlen(name); } // guest pointers used as host pointers

struct Env final : UserCallbacks {
    Jit* jit = nullptr;
    template <class T> T rd(uint64_t a) { T v; memcpy(&v, (void*)a, sizeof v); return v; }
    template <class T> void wr(uint64_t a, T v) { memcpy((void*)a, &v, sizeof v); }
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
    void InterpreterFallback(VAddr pc, size_t n) override { fprintf(stderr, "fallback pc=%llx n=%zu\n", (unsigned long long)pc, n); exit(2); }
    void ExceptionRaised(VAddr pc, Exception e) override { fprintf(stderr, "exception %d pc=%llx\n", (int)e, (unsigned long long)pc); exit(2); }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1ull << 40; }
    uint64_t GetCNTPCT() override { return 19200000; }
    void CallSVC(uint32_t swi) override {
        if (swi == 1) { jit->HaltExecution(); return; }
        if (swi == 0x42) { // hle_add(x0, x1, x2) -> x0
            jit->SetRegister(0, hle_add(jit->GetRegister(0), jit->GetRegister(1), jit->GetRegister(2)));
        } else if (swi == 0x43) { // hle_poke(x0 = guest ptr, x1 = guest string)
            hle_poke((uint32_t*)jit->GetRegister(0), (const char*)jit->GetRegister(1));
        }
    }
};

int main() {
    void* p = mmap((void*)kBase, kSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void*)kBase) { perror("mmap fixed"); return 1; }

    const uint32_t prog[] = {
        0xD28000A0, // movz x0, #5
        0xD28000E1, // movz x1, #7
        0xD2800062, // movz x2, #3
        0xD4000841, // svc #0x42          x0 = hle_add(5, 7, 3) = 15
        0xF9000060, // str x0, [x3]
        0xAA0403E0, // mov x0, x4         out ptr
        0xAA0503E1, // mov x1, x5         string ptr
        0xD4000861, // svc #0x43          hle_poke
        0xD53BD046, // mrs x6, tpidrro_el0
        0xD53BD047, // mrs x7, tpidr_el0
        0xD53BE028, // mrs x8, cntpct_el0
        0xD4000021, // svc #1 (halt)
    };
    memcpy((void*)kBase, prog, sizeof prog);               // code at kBase
    uint64_t data = kBase + 0x1000, out = kBase + 0x1010, str = kBase + 0x1020;
    strcpy((char*)str, "hello_nx");

    Env env;
    UserConfig cfg{};
    cfg.callbacks = &env;
    cfg.fastmem_pointer = 0;                 // host address == guest address
    cfg.fastmem_address_space_bits = 48;
    cfg.silently_mirror_fastmem = false;
    uint64_t tpidr = 0xAAAA, tpidrro = 0xBBBB;
    cfg.tpidr_el0 = &tpidr; cfg.tpidrro_el0 = &tpidrro;
    cfg.wall_clock_cntpct = true;
    cfg.enable_cycle_counting = false;
    Jit jit(cfg);
    env.jit = &jit;
    jit.SetPC(kBase);
    jit.SetSP(kBase + 0x8000);
    jit.SetRegister(3, data); jit.SetRegister(4, out); jit.SetRegister(5, str);
    HaltReason hr = jit.Run();

    uint64_t stored = *(uint64_t*)data; uint32_t len = *(uint32_t*)out;
    printf("halt=%x hle_calls=%d x0=%llu stored=%llu strlen=%u tpidrro=%llx tpidr=%llx cntpct=%llu\n",
           (unsigned)hr, g_hle_calls, (unsigned long long)jit.GetRegister(0), (unsigned long long)stored,
           len, (unsigned long long)jit.GetRegister(6), (unsigned long long)jit.GetRegister(7),
           (unsigned long long)jit.GetRegister(8));
    bool ok = stored == 15 && len == 8 && jit.GetRegister(6) == 0xBBBB && jit.GetRegister(7) == 0xAAAA && jit.GetRegister(8) == 19200000;
    puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
