// The AArch32 side of the guest CPU (cpu.h): the same API, selected for the whole process by cpu_set_aarch32(1) before the first cpu_create.
// Register numbering for a 32-bit guest: x0..x12 are r0..r12, x13 and sp are r13, x14 and x30 are lr (r14), pc is r15, v0..v15 are q0..q15. While the kernel handles an SVC, x0..x7
// are the AArch64-layout arguments svc32.c built from the guest's registers (and the results it leaves there are written back to r0..r7 afterwards).
#pragma once
#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif
uint32_t cpu32_call(cpu_t *c, uint32_t fn, const uint32_t *args, int nargs);
void cpu32_set_return_pad(uint32_t addr);   // a guest address holding `svc #0x7FE`: where a guest function called by cpu32_call returns to
void cpu32_set_trap(uint32_t addr);            // NX_TRAP: replace the ARM `mov rd, rm` at addr by a trap that logs the registers
void cpu32_set_window(uint64_t lo, uint64_t hi);   // addresses outside [lo, hi) are reported and stop the thread instead of crashing the host
cpu_t *cpu32_create(const cpu_ops_t *ops, void *user, unsigned core);
void cpu32_destroy(cpu_t *c);
int cpu32_run(cpu_t *c);
void cpu32_halt(cpu_t *c);
uint64_t cpu32_get_x(cpu_t *c, int i);
void cpu32_set_x(cpu_t *c, int i, uint64_t v);
uint64_t cpu32_get_pc(cpu_t *c);
void cpu32_set_pc(cpu_t *c, uint64_t v);       // bit 0 set = Thumb
uint64_t cpu32_get_sp(cpu_t *c);
void cpu32_set_sp(cpu_t *c, uint64_t v);
void cpu32_set_tls(cpu_t *c, uint64_t tpidrurw, uint64_t tpidruro);
void cpu32_get_v(cpu_t *c, int i, uint64_t out[2]);
void cpu32_set_v(cpu_t *c, int i, const uint64_t in[2]);
void cpu32_invalidate(cpu_t *c, uint64_t addr, uint64_t len);
#ifdef __cplusplus
}
#endif
