# nxlinux

Linux x86_64 port of nxnative: an AArch64 JIT (dynarmic) runs the game's code, a Horizon HLE host answers its system calls, and NVN is
implemented on Vulkan. Plan: `docs/plan.md`. Port notes: `docs/nvn-port-analysis.md`, `docs/vulkan-notes.md`.

## This checkout is shared by several agents. Read `docs/agents/README.md` first. In short:
- Start with `python3 tools/agentmsg.py --as NAME inbox` and `... status`. NAME is your session name.
- Never `git add -A` / `git commit -a` here. Commit with `python3 tools/agentmsg.py --as NAME commit -m "one line" PATH...`.
- Claim before editing another agent's area (`claim PATH...`); message instead of overwriting.
- Build in your own directory: `cmake -S . -B build-NAME -G Ninja && cmake --build build-NAME && (cd build-NAME && ctest)`.
- Commit messages are one plain line and never mention an agent or an AI.
- No game files, keys or firmware in the repo. Use `tools/synth_nsp.py` for stand-ins.
