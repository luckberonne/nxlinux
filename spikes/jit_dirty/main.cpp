// Spike: write-protect tracking (as dm/gfx/nvn/nvn_dirty.c does with mprotect + SIGSEGV) under dynarmic fastmem.
// A guest store to a read-only page faults inside JIT code; dynarmic must route it to the MemoryWrite callback,
// Finding: dynarmic installs its SIGSEGV handler in the Jit constructor; ours, installed after, sees every fault first
// and must chain non-tracked ones to it (case 3). Original expectation, kept for reference: the write callback sees the
// guest store; in practice our handler does. A host-side store to a protected
// page (HLE code) must reach our own SIGSEGV handler instead.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <sys/mman.h>
#include <signal.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace Dynarmic;
using namespace Dynarmic::A64;

static constexpr uint64_t kBase = 0x2000000000ull;
static constexpr size_t kSize = 64ull << 20;
static constexpr uint64_t kPage = 0x4000;                 // 16 KiB host pages on Apple; 4 KiB here, tracked at 16 KiB
static constexpr uint64_t kTracked = kBase + 0x10000;

static volatile int g_cb_faults, g_host_faults, g_hole_hits;
static constexpr uint64_t kHole = kBase + 0x20000;      // PROT_NONE: a page the guest should not reach
static struct sigaction g_prev;                          // dynarmic's handler

static void unprotect(uint64_t a) { mprotect((void*)(a & ~(kPage - 1)), kPage, PROT_READ | PROT_WRITE); }
static bool tracked(uint64_t a) { return a >= kTracked && a < kTracked + kPage; }

static void host_segv(int sig, siginfo_t* si, void* ctx) {      // our own handler, as crash.c / nvn_dirty.c install
    uint64_t a = (uint64_t)si->si_addr;
    if (!tracked(a)) {                                   // not ours: chain to the previous handler (dynarmic)
        if (g_prev.sa_flags & SA_SIGINFO) g_prev.sa_sigaction(sig, si, ctx); else signal(SIGSEGV, SIG_DFL);
        return;
    }
    g_host_faults++;
    unprotect(a);
}

struct Env final : UserCallbacks {
    Jit* jit = nullptr;
    template <class T> T rd(uint64_t a) { T v; memcpy(&v, (void*)a, sizeof v); return v; }
    template <class T> void wr(uint64_t a, T v) {
        if (a >= kHole && a < kHole + kPage) { g_hole_hits++; return; }
        if (tracked(a)) { g_cb_faults++; unprotect(a); }
        memcpy((void*)a, &v, sizeof v);
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
    void InterpreterFallback(VAddr, size_t) override { exit(2); }
    void ExceptionRaised(VAddr pc, Exception e) override { fprintf(stderr, "exception %d pc=%llx\n", (int)e, (unsigned long long)pc); exit(2); }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1ull << 40; }
    uint64_t GetCNTPCT() override { return 0; }
    void CallSVC(uint32_t swi) override { if (swi == 1) jit->HaltExecution(); }
};

int main() {
    if (mmap((void*)kBase, kSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void*)kBase) { perror("mmap"); return 1; }

    Env env;
    UserConfig cfg{};
    cfg.callbacks = &env;
    cfg.fastmem_pointer = 0;
    cfg.fastmem_address_space_bits = 48;
    cfg.silently_mirror_fastmem = false;
    cfg.enable_cycle_counting = false;
    cfg.wall_clock_cntpct = true;
    Jit jit(cfg);                                        // dynarmic installs its handler here
    env.jit = &jit;

    struct sigaction sa{}; sa.sa_sigaction = host_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    struct sigaction old; sigaction(SIGSEGV, &sa, &old); g_prev = old; printf("old handler before ours: %p\n", (void*)old.sa_sigaction);  // installed after the JIT, so it sees non-JIT faults first
    // NOTE: dynarmic installs its handler on first use of a Jit; if ours must run first it has to chain. Tested below.

    const uint32_t prog[] = {
        0xF9000020, // str x0, [x1]
        0xD4000021, // svc #1
    };
    memcpy((void*)kBase, prog, sizeof prog);

    // Case 1: guest store to a protected page
    mprotect((void*)kTracked, kPage, PROT_READ);
    jit.SetPC(kBase); jit.SetSP(kBase + 0x8000); jit.SetRegister(0, 0x1234); jit.SetRegister(1, kTracked + 0x40);
    jit.Run();
    { struct sigaction cur; sigaction(SIGSEGV, nullptr, &cur); printf("handler after Run is ours: %d\n", cur.sa_sigaction == host_segv); }
    uint64_t v1 = *(uint64_t*)(kTracked + 0x40);
    printf("guest store: cb_faults=%d host_faults=%d value=%llx\n", g_cb_faults, g_host_faults, (unsigned long long)v1);

    // Case 2: host-side store to a protected page (HLE code writing guest memory)
    int rc = mprotect((void*)kTracked, kPage, PROT_READ);
    printf("mprotect rc=%d before host store: host_faults=%d\n", rc, g_host_faults);
    *(volatile uint64_t*)(kTracked + 0x80) = 0x77;
    printf("host store: cb_faults=%d host_faults=%d value=%llx\n", g_cb_faults, g_host_faults, (unsigned long long)*(uint64_t*)(kTracked + 0x80));

    // Case 3: guest store to a PROT_NONE page must reach dynarmic through our chain and end in the callback
    mprotect((void*)kHole, kPage, PROT_NONE);
    jit.SetPC(kBase); jit.SetRegister(0, 0x99); jit.SetRegister(1, kHole + 8);
    jit.Run();
    printf("hole store: hole_hits=%d host_faults=%d\n", g_hole_hits, g_host_faults);
    bool ok = g_cb_faults == 0 && v1 == 0x1234 && g_host_faults == 2 && g_hole_hits == 1;
    puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
