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

enum { CPU_HALT_NONE = 0, CPU_HALT_REQUESTED = 1, CPU_HALT_FAULT = 2 };

// core: 0..cpu_max_cores-1, the exclusive-monitor slot of this thread (one per concurrently running guest thread).
enum { CPU_MAX_CORES = 256 };
cpu_t *cpu_create(const cpu_ops_t *ops, void *user, unsigned core);
void cpu_destroy(cpu_t *c);

int cpu_run(cpu_t *c);                 // runs until cpu_halt() or a fault; returns a CPU_HALT_* value
void cpu_halt(cpu_t *c);               // callable from any thread, or from inside the svc handler

uint64_t cpu_get_x(cpu_t *c, int i);
void cpu_set_x(cpu_t *c, int i, uint64_t v);
uint64_t cpu_get_pc(cpu_t *c);
void cpu_set_pc(cpu_t *c, uint64_t v);
uint64_t cpu_get_sp(cpu_t *c);
void cpu_set_sp(cpu_t *c, uint64_t v);
void cpu_set_tls(cpu_t *c, uint64_t tpidr_el0, uint64_t tpidrro_el0);
void cpu_get_v(cpu_t *c, int i, uint64_t out[2]);
void cpu_set_v(cpu_t *c, int i, const uint64_t in[2]);
void cpu_invalidate(cpu_t *c, uint64_t addr, uint64_t len);   // the guest wrote code there

#ifdef __cplusplus
}
#endif
