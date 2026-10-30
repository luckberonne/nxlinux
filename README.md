# nxlinux

Port of [nxnative](https://github.com/seedds/nxnative) to Linux x86_64: an AArch64 JIT (dynarmic) runs the game's code,
a Horizon HLE host answers its system calls, and NVN is implemented on Vulkan.
First target: Mario Kart 8 Deluxe. Work in progress; see `docs/plan.md`.

Mario Kart 8 Deluxe 4.0.1 boots and shows its title splash: see `docs/mk8d-bringup.md` (how to run it, what a retail title needs from the host, debugging switches, what is missing).
