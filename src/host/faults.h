// Page-fault dispatch for write-protect dirty tracking (nvn_dirty.c) and the host's own guards.
// dynarmic installs its SIGSEGV handler when a Jit is created; ours must sit on top of it and chain whatever is not ours
// (a guest access to unmapped memory must still reach dynarmic and end in the memory callbacks). cpu.cpp calls the
// hook below after it creates a Jit, which re-installs ours on top.
#pragma once
void fault_install(void);   // idempotent: puts our handler in front of whatever is installed now and chains to it
