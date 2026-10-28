# Phase 0 findings

Dynarmic: `azahar-emu/dynarmic` @ a466015 (0BSD, A64 frontend). Upstream `merryhime/dynarmic` is gone. The CMake target is
`dynarmic` (no `dynarmic::dynarmic` alias). Needs Boost headers.

## Address model (spikes/jit_hello): works
Reserving the guest window with `mmap(MAP_FIXED_NOREPLACE)` at 0x20'0000'0000 and setting `fastmem_pointer = 0`,
`fastmem_address_space_bits = 48`, `silently_mirror_fastmem = false` makes guest address == host address. A guest
`svc` reaches `CallSVC`, host C functions take guest pointers directly (`strlen` on a guest string, a store through a
guest pointer), and `tpidr_el0`, `tpidrro_el0` and `cntpct_el0` work through `UserConfig` / `GetCNTPCT`.
Consequence: the HLE code that dereferences guest pointers can be reused as is. No `G2H()` rewrite.

## Dirty tracking (spikes/jit_dirty): works with two rules
1. Install our SIGSEGV handler after the first `Jit` is constructed (dynarmic installs its own in the constructor).
2. Chain every fault that is not ours to the previous handler. A guest store to a PROT_NONE page then ends in
   `MemoryWrite*` through dynarmic, as expected.
Guest stores to write-protected tracked pages fault in JIT code and reach our handler, which unprotects and retries.

## HLE call shim (src/host/hle_call.c): works
`hle_call(fn, regs)` maps AAPCS64 argument registers to a SysV call: x0..x5 to rdi, rsi, rdx, rcx, r8, r9, x6 and x7 on
the stack, d0..d7 to xmm0..7, and brings x0/d0 back. Tested with 8 integer arguments, mixed int/double/float, a float
return, a pointer out-parameter and garbage in the upper half of a W register. It must be pure assembly: an inline
`asm` version crashed because the compiler can place inputs in registers the sequence overwrites. Not covered:
variadics, structs by value, x8 indirect results.

## Still open
- Host page size is 4 KiB here; the Apple code assumes 16 KiB. Tracking granularity must be revisited in `nvn_dirty.c`.

## Jit per guest thread does not scale (found while building the kernel)
Creating a dynarmic `Jit` takes ~18 ms and every Jit has its own code cache, so one Jit per guest thread would
retranslate all code for every thread. `src/host/cpu.cpp` instead keeps a pool of Jits (NX_JITS, default 8) and a saved
register context per guest thread. A thread borrows a Jit only while it runs guest code: `svc` leaves `Run()`
(CallSVC halts with UserDefined2), the kernel handles it outside, where it may block without holding a Jit, and the
thread resumes on any free Jit. Creating a guest thread is now free. Pool ids are the exclusive-monitor slots and must be
reserved under the lock (two Jits with the same id lost atomic updates).

## Memory SVCs (src/host/mem.c)
The window is partitioned [low | alias | stack | heap]; everything above the low region is PROT_NONE until committed.
Host pages are 4 KiB here, like Horizon's, so nxnative's 16 KiB rounding, "exposed page" tracking and eager zeroing are gone:
unmapping and heap shrinking `madvise(MADV_DONTNEED)`, which makes remapped memory read as zero. Implemented: SetHeapSize
(OutOfMemory 0xD001 past the region, LimitReached 0x10801 past the pool less the images), SetMemoryPermission/Attribute,
MapMemory/UnmapMemory (copy semantics, borrowed sources tracked, shrink/permission changes on a borrowed source refused),
QueryMemory (heap, mappings, alias runs, free gaps, modules in Horizon's Code RX / Code R / CodeData RW layout, host-made
stacks as Stack), Map/UnmapPhysicalMemory (alias page bitmap, optional pool limit), GetInfo, Break (bit 31 = notification),
OutputDebugString and GetSystemTick. GetSystemTick returns the tick count in x0 itself (the ABI), where nxnative returned
it in x1 behind a Result.
Not ported (Apple- or diagnostic-specific): address-region tracking via SDK gates, the Unreal heap zone, NRO deactivation.
