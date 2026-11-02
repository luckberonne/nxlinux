// AArch64 guest CPU: one cpu_t per guest thread, all sharing the guest window (guest address == host address)
// and one exclusive monitor, so ldxr/stxr between threads is atomic.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cpu cpu_t;

typedef struct {
    void (*svc)(cpu_t *c, uint32_t swi, void *user);   // guest executed `svc #swi`; arguments in x0..x7
    uint64_t (*cntpct)(void);                          // system counter, 19.2 MHz on the Switch
    void (*fault)(cpu_t *c, uint64_t pc, int kind, void *user);   // unmapped code, undefined instruction, etc.
} cpu_ops_t;

// An AArch32 process (main.npdm's 64-bit flag clear) runs every cpu_t on the A32 Jits in cpu32.cpp. Call cpu_set_aarch32(1) before the first cpu_create; the API below then reads
// and writes the 32-bit registers, see cpu32.h.
void cpu_set_aarch32(int on);
// Runs a guest function from inside an SVC handler (a host function the guest called): fn(args[0..nargs-1], up to 8) on this thread's cpu, then returns its r0. Nests: the guest function
// may itself make SVCs and call hooked functions. AArch32 only (returns 0 otherwise).
uint32_t cpu_call32(cpu_t *c, uint32_t fn, const uint32_t *args, int nargs);
uint64_t cpu_call64(cpu_t *c, uint64_t fn, const uint64_t *args, int nargs);   // the AArch64 equivalent: x0..x7 = args, returns x0 (kernel_init sets the return pad)
void cpu_set_return_pad64(uint64_t addr);
void cpu_set_trap64(uint64_t addr);   // NX_TRAP on a 64-bit title: replace the `mov xd, xm` at addr by a register-logging trap
int cpu_is_aarch32(void);

enum { CPU_HALT_NONE = 0, CPU_HALT_REQUESTED = 1, CPU_HALT_FAULT = 2 };

// core: 0..cpu_max_cores-1, the exclusive-monitor slot of this thread (one per concurrently running guest thread).
enum { CPU_MAX_CORES = 256 };
cpu_t *cpu_create(const cpu_ops_t *ops, void *user, unsigned core);
void cpu_destroy(cpu_t *c);

int cpu_run(cpu_t *c);                 // runs until cpu_halt() or a fault; returns a CPU_HALT_* value
void cpu_halt(cpu_t *c);               // callable from any thread, or from inside the svc handler
void cpu_set_preempt_hook(void (*fn)(cpu_t *));   // AArch64: called on the guest thread's own host thread, outside the Jit, after cpu_preempt stopped it
int cpu_translating(cpu_t *c, uint64_t *pc, uint64_t *count);   // thread dump: the last instruction address the Jit's translator read for this cpu, and how many it has read in all
void cpu_preempt(cpu_t *c);            // callable from any thread: the guest thread stops at the end of its block, fn runs, the guest continues

uint64_t cpu_get_x(cpu_t *c, int i);
void cpu_set_x(cpu_t *c, int i, uint64_t v);
uint64_t cpu_get_pc(cpu_t *c);
void cpu_set_pc(cpu_t *c, uint64_t v);
uint64_t cpu_get_sp(cpu_t *c);
void cpu_set_sp(cpu_t *c, uint64_t v);
void cpu_set_tls(cpu_t *c, uint64_t tpidr_el0, uint64_t tpidrro_el0);
void cpu_get_v(cpu_t *c, int i, uint64_t out[2]);
void cpu_set_v(cpu_t *c, int i, const uint64_t in[2]);
void cpu_invalidate(cpu_t *c, uint64_t addr, uint64_t len);
// Called after every new Jit is created (dynarmic installs its fault handler then); faults.c uses it to put its own back on top
void cpu_set_jit_created_hook(void (*fn)(void));   // the guest wrote code there

#ifdef __cplusplus
}
#endif
