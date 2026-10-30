// AArch32 guests: the A32 Jit runs ARM code in the low window, cp15 serves TPIDRURO and CNTPCT, and the SVC layouts of svc32.c arrive in the AArch64 order the kernel handlers read.
// The program is tests/guest-style hand assembly (llvm-mc --triple=armv7a); each svc leaves the registers the title's wrappers would.
extern "C" {
#include "../src/host/guest_mem.h"
#include "../src/host/cpu.h"
#include "../src/host/svc32.h"
}
#include <cstdio>
#include <cstring>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long _a = (a), _b = (b); if (_a != _b) { printf("FAIL line %d: %s = %#llx, expected %#llx\n", __LINE__, #a, _a, _b); g_fail = 1; } } while (0)

static const uint32_t PROG[] = {
    0xe3050678, 0xe3400001, 0xe3a01002, 0xef00000b,                         // r0 = 0x15678, r1 = 2; svc 0x0b  SleepThread(2<<32 | 0x15678)
    0xe3a0002c, 0xe3a01c01, 0xe3a02c02, 0xe3a03c03, 0xe3a04001, 0xef000008, // r0 = 44 (priority), r1 = 0x100, r2 = 0x200, r3 = 0x300, r4 = 1; svc 0x08  CreateThread
    0xe1a05000, 0xe1a06001,                                                 // r5 = result, r6 = handle
    0xee1d7f70,                                                             // mrc p15, 0, r7, c13, c0, 3  (TPIDRURO)
    0xec598f0e,                                                             // mrrc p15, 0, r8, r9, c14   (CNTPCT)
    0xe3a00007, 0xe3a0100b, 0xe3a02000, 0xe3a03009, 0xef000029,            // GetInfo(type 11, handle 0, subtype 9<<32 | 7)
    0xe1a0a001, 0xe1a0b002,                                                 // r10:r11 = the value
    0xef0007f0,                                                             // svc 0x7f0: the test's stop
};

static int g_seen;
static void on_svc(cpu_t *c, uint32_t swi, void *) {
    if (swi == 0x0b) { g_seen |= 1; CHECK_EQ(cpu_get_x(c, 0), 0x200015678ull); }
    else if (swi == 0x08) {
        g_seen |= 2;
        CHECK_EQ(cpu_get_x(c, 1), 0x100); CHECK_EQ(cpu_get_x(c, 2), 0x200); CHECK_EQ(cpu_get_x(c, 3), 0x300);
        CHECK_EQ(cpu_get_x(c, 4), 44); CHECK_EQ(cpu_get_x(c, 5), 1);
        cpu_set_x(c, 0, 0); cpu_set_x(c, 1, 0xABCD);
    } else if (swi == 0x29) {
        g_seen |= 4;
        CHECK_EQ(cpu_get_x(c, 1), 11); CHECK_EQ(cpu_get_x(c, 2), 0); CHECK_EQ(cpu_get_x(c, 3), 0x900000007ull);
        cpu_set_x(c, 0, 0); cpu_set_x(c, 1, 0xAABBCCDD11223344ull);
    } else if (swi == 0x7f0) { g_seen |= 8; cpu_halt(c); }
}
static uint64_t cntpct() { return 0x1122334455667788ull; }
static void on_fault(cpu_t *, uint64_t pc, int kind, void *) { printf("fault kind %d at %#llx\n", kind, (unsigned long long)pc); g_fail = 1; }

static void test_layouts() {
    uint32_t r[16] = {}; uint64_t x[8]; svc32_ctx_t cx;
    // WaitSynchronization(→r1 index; r1 handles, r2 count, timeout r0:r3): the timeout takes the registers the pointers left free
    r[0] = 0x11; r[1] = 0x2000; r[2] = 3; r[3] = 0x22;
    svc32_enter(0x18, r, x, &cx);
    CHECK_EQ(x[1], 0x2000); CHECK_EQ(x[2], 3); CHECK_EQ(x[3], 0x2200000011ull);
    uint64_t out[8] = {0xE401, 5};
    svc32_leave(0x18, out, r, &cx);
    CHECK_EQ(r[0], 0xE401); CHECK_EQ(r[1], 5);
    // GetThreadCoreMask: the 64-bit mask comes back in r2:r3
    uint64_t o2[8] = {0, 1, 0x700000003ull};
    svc32_leave(0x0e, o2, r, &cx);
    CHECK_EQ(r[1], 1); CHECK_EQ(r[2], 3); CHECK_EQ(r[3], 7);
    // an SVC with no entry passes r0..r7 through, for the kernel to report
    r[0] = 1; r[5] = 9;
    svc32_enter(0x7e, r, x, &cx);
    CHECK_EQ(x[0], 1); CHECK_EQ(x[5], 9);
    CHECK(svc32_known(0x08) && !svc32_known(0x7e));
}

static void test_hooks(uint32_t *stack) {       // stack: guest memory, below 4 GiB
    uint32_t r[16] = {}; uint64_t x[8], fv[8]; svc32_ctx_t cx;
    for (int i = 0; i < 8; i++) stack[i] = 0xA0 + i;
    r[13] = (uint32_t)(uintptr_t)stack;
    r[0] = 1; r[1] = 2; r[2] = 3; r[3] = 4;
    // one-word parameters take r0..r3, then the stack
    CHECK_EQ(svc32_register_hook(0x1000, "_ZN2nn2oe25TryPopNotificationMessageEPj"), 1);
    CHECK_EQ(svc32_register_hook(0x1001, "_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi"), 2);
    CHECK_EQ(svc32_register_hook(0x1002, "_ZN2nn2sf4hipc6client18DuplicateSessionExEPNS_3svc6HandleES4_j"), 3);   // P.., S4_, j
    CHECK_EQ(svc32_register_hook(0x1003, "_Z3fooiiiiii"), 6);
    CHECK_EQ(svc32_register_hook(0x1004, "_Z3fooixi"), 3);
    CHECK_EQ(svc32_register_hook(0x1005, "_ZN2nn2oe10InitializeEv"), 0);
    CHECK_EQ(svc32_register_hook(0x1006, "_ZNSt3__16vectorIiNS_9allocatorIiEEE9push_backERKi"), 1);   // a member of a template class
    CHECK_EQ(svc32_register_hook(0x1007, "_ZN2nn3foo3barIiEEvT_"), 1);                              // a function template: the return type comes first
    svc32_enter(0x1003, r, x, &cx);
    CHECK_EQ(x[0], 1); CHECK_EQ(x[3], 4); CHECK_EQ(x[4], 0xA0); CHECK_EQ(x[5], 0xA1);
    // a 64-bit integer takes an even register pair: foo(int a, long long b, int c) → a r0, b r2:r3, c on the stack
    svc32_enter(0x1004, r, x, &cx);
    CHECK_EQ(x[0], 1); CHECK_EQ(x[1], 0x0000000400000003ull); CHECK_EQ(x[2], 0xA0);
    // foo(long long a, int b) → a r0:r1, b r2
    CHECK_EQ(svc32_register_hook(0x1008, "_Z3barxi"), 2);
    svc32_enter(0x1008, r, x, &cx);
    CHECK_EQ(x[0], 0x0000000200000001ull); CHECK_EQ(x[1], 3);
    // DuplicateSessionEx(P, P, j): r0, r1, r2
    svc32_enter(0x1002, r, x, &cx);
    CHECK_EQ(x[0], 1); CHECK_EQ(x[1], 2); CHECK_EQ(x[2], 3);
    // the result returns in r0 (low word) and r1 (high word)
    uint64_t res[8] = {0x1122334455667788ull};
    svc32_leave(0x1004, res, r, &cx);
    CHECK_EQ(r[0], 0x55667788); CHECK_EQ(r[1], 0x11223344);
    r[0] = 1; r[1] = 2; r[2] = 3; r[3] = 4;
    // VFP arguments (hard-float): floats and doubles take s0../d0.. and reach the host in v0.. in order; the integers around them stay in x0.. in order
    uint32_t ext[64] = {};
    ext[0] = 0x3f800000; ext[1] = 0x40000000; ext[2] = 0; ext[3] = 0x40080000;     // s0 = 1.0f, s1 = 2.0f, d1 (s2:s3) = 3.0
    CHECK_EQ(svc32_register_sig(0x100a, "iff", 'v'), 3);
    svc32_enter2(0x100a, r, ext, x, fv, &cx);
    CHECK_EQ(x[0], 1); CHECK_EQ(fv[0], 0x3f800000); CHECK_EQ(fv[1], 0x40000000);
    CHECK_EQ(svc32_register_sig(0x100b, "fdf", 'v'), 3);     // f -> s0, d -> d1 (s2:s3), f -> s1 (back-fills the hole)
    svc32_enter2(0x100b, r, ext, x, fv, &cx);
    CHECK_EQ(fv[0], 0x3f800000); CHECK_EQ(fv[1], 0x4008000000000000ull); CHECK_EQ(fv[2], 0x40000000);
    CHECK_EQ(svc32_register_hook(0x100c, "_Z3fooifd"), 3);
    svc32_enter2(0x100c, r, ext, x, fv, &cx);
    CHECK_EQ(x[0], 1); CHECK_EQ(fv[0], 0x3f800000); CHECK_EQ(fv[1], 0x4008000000000000ull);
    // a float result comes back in s0, a double in s0:s1, a 64-bit integer in r0:r1
    CHECK_EQ(svc32_register_sig(0x100d, "i", 'f'), 1);
    uint32_t ext2[64] = {}; uint64_t zero[8] = {};
    svc32_leave2(0x100d, zero, 0x40490fdbull, r, ext2, &cx);
    CHECK_EQ(ext2[0], 0x40490fdb);
    CHECK_EQ(svc32_register_sig(0x100e, "i", 'd'), 1);
    svc32_leave2(0x100e, zero, 0x400921fb54442d18ull, r, ext2, &cx);
    CHECK_EQ(ext2[0], 0x54442d18); CHECK_EQ(ext2[1], 0x400921fb);
    CHECK_EQ(svc32_register_sig(0x100f, "iixi", 'x'), 4);     // an NVN-style signature: (ptr, int, u64 address, size)
    svc32_enter2(0x100f, r, ext, x, fv, &cx);
    CHECK_EQ(x[0], 1); CHECK_EQ(x[1], 2); CHECK_EQ(x[2], 0x0000000400000003ull); CHECK_EQ(x[3], 0xA0);
    // a hook that returns an 8-byte class: r0 is the caller's buffer, the arguments start at r1, and the result is written to the buffer
    CHECK_EQ(svc32_register_hook(0x1010, "_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE"), 1);
    r[0] = (uint32_t)(uintptr_t)(stack + 4); r[1] = 0xCAFE;
    svc32_enter2(0x1010, r, NULL, x, fv, &cx);
    CHECK_EQ(x[0], 0xCAFE);
    uint64_t sres[8] = {0x1122334455667788ull};
    svc32_leave2(0x1010, sres, 0, r, NULL, &cx);
    CHECK_EQ(r[0], (uint32_t)(uintptr_t)(stack + 4)); CHECK_EQ(stack[4], 0x55667788); CHECK_EQ(stack[5], 0x11223344);
    r[0] = 9; r[1] = 2; r[2] = 3; r[3] = 4;
    // a C name, and a stub nobody registered, are eight one-word parameters
    CHECK_EQ(svc32_register_hook(0x1009, "nvnBootstrapLoader"), -1);
    r[0] = 9; svc32_enter(0x1009, r, x, &cx); CHECK_EQ(x[0], 9); CHECK_EQ(x[4], 0xA0);
    svc32_enter(0x1ff0, r, x, &cx); CHECK_EQ(x[0], 9); CHECK_EQ(x[7], 0xA3);
}

int main() {
    test_layouts();
    cpu_set_aarch32(1);
    CHECK(guest_mem_init32(64ull << 20) == 0);
    CHECK_EQ(guest_mem_base(), 0x200000);
    uint64_t code = guest_alloc(0x1000, 0x1000);
    CHECK_EQ(code, 0x200000);                      // the first allocation is at 0x200000, where rtld sits
    memcpy((void *)code, PROG, sizeof PROG);
    uint64_t stack = guest_alloc(0x1000, 0x1000);
    test_hooks((uint32_t *)stack);
    cpu_ops_t ops = {on_svc, cntpct, on_fault};
    cpu_t *c = cpu_create(&ops, nullptr, 0);
    cpu_set_pc(c, code); cpu_set_sp(c, stack + 0x1000);
    cpu_set_tls(c, 0x7777, 0x5000);
    int rc = cpu_run(c);
    CHECK_EQ(rc, CPU_HALT_REQUESTED);
    CHECK_EQ(g_seen, 15);
    CHECK_EQ(cpu_get_x(c, 5), 0); CHECK_EQ(cpu_get_x(c, 6), 0xABCD);   // CreateThread's result and handle came back in r0 and r1
    CHECK_EQ(cpu_get_x(c, 7), 0x5000);                                 // TPIDRURO
    CHECK_EQ(cpu_get_x(c, 8), 0x55667788); CHECK_EQ(cpu_get_x(c, 9), 0x11223344);   // CNTPCT in r8:r9
    CHECK_EQ(cpu_get_x(c, 10), 0x11223344); CHECK_EQ(cpu_get_x(c, 11), 0xAABBCCDD); // GetInfo's 64-bit value in r1:r2
    cpu_destroy(c);
    printf(g_fail ? "FAIL\n" : "ok\n");
    return g_fail;
}
