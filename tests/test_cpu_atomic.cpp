// Two guest threads (two cpu_t on two host threads) each add 1 to a shared counter N times with ldxr/stxr.
// The total must be exactly 2N, and ldaxr/stlxr must work too. Also checks svc dispatch and halt.
#include "../src/host/cpu.h"
#include <sys/mman.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>

static constexpr uint64_t kBase = 0x2000000000ull;
static constexpr uint64_t kCode = kBase, kCounter = kBase + 0x10000;
static constexpr int N = 200000;

static void on_svc(cpu_t *c, uint32_t swi, void *) { if (swi == 1) cpu_halt(c); }
static uint64_t cnt(void) { return 0; }

static void *worker(void *arg) {
    unsigned core = (unsigned)(uintptr_t)arg;
    cpu_ops_t ops = {on_svc, cnt, nullptr};
    cpu_t *c = cpu_create(&ops, nullptr, core);
    cpu_set_pc(c, kCode);
    cpu_set_sp(c, kBase + 0x40000 + core * 0x10000);
    cpu_set_x(c, 0, kCounter);
    cpu_set_x(c, 1, N);
    int r = cpu_run(c);
    if (r != CPU_HALT_REQUESTED) printf("core %u: run returned %d\n", core, r);
    cpu_destroy(c);
    return nullptr;
}

int main() {
    if (mmap((void *)kBase, 64ull << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void *)kBase) { perror("mmap"); return 1; }
    const uint32_t prog[] = {
        // loop: ldaxr x2,[x0]; add x2,x2,#1; stlxr w3,x2,[x0]; cbnz w3,loop; subs x1,x1,#1; b.ne loop; svc #1
        0xC85FFC02, 0x91000442, 0xC803FC02, 0x35FFFFA3, 0xF1000421, 0x54FFFF61, 0xD4000021,
    };
    memcpy((void *)kCode, prog, sizeof prog);
    pthread_t t[2];
    for (int i = 0; i < 2; i++) pthread_create(&t[i], nullptr, worker, (void *)(uintptr_t)i);
    for (int i = 0; i < 2; i++) pthread_join(t[i], nullptr);
    uint64_t total = *(volatile uint64_t *)kCounter;
    printf("counter=%llu expected=%d\n", (unsigned long long)total, 2 * N);
    puts(total == 2 * N ? "PASS" : "FAIL");
    return total == 2 * N ? 0 : 1;
}
