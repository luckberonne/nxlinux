# nxlinux

Linux x86_64 port of nxnative: an AArch64 JIT (dynarmic) runs the game's code, a Horizon HLE host answers its system calls, and NVN is
implemented on Vulkan. Plan: `docs/plan.md`. Port notes: `docs/nvn-port-analysis.md`, `docs/vulkan-notes.md`.

## This checkout is shared by several agents, built on different tools (not all of them Claude), and by the owner
Whatever you are, you work in the same working tree as the others. Read `docs/agents/README.md` (five minutes). The short version:

1. **Start with one command**: `python3 tools/agentmsg.py --as NAME start`. It prints the rules, who has claimed what, and your unread
   mail. NAME is a short stable identifier for you (your tool's name is fine: `codex`, `gemini`, `cursor`, ...). Reuse it every time, and
   tell the others who you are with `send --to all -m "I am NAME, working on ..."`. Add `--json` if you are a program.
2. **Never `git add -A`, `git add .` or `git commit -a`** here: they take other agents' unfinished files with yours. Commit with
   `python3 tools/agentmsg.py --as NAME commit -m "one line" PATH...`; it commits exactly those paths and refuses paths someone else claimed.
3. **Claim before you edit** anything outside your own area: `python3 tools/agentmsg.py --as NAME claim PATH... --note "why"`; release after.
   To ask something of another agent, `send --to THEM --kind question -m "..."`; answers come back through `inbox`.
4. **Build in your own directory**: `cmake -S . -B build-NAME -G Ninja && cmake --build build-NAME && (cd build-NAME && ctest)`.
   Never in `build/`.
5. Commit messages are one plain line and never mention an agent or an AI. No game files, keys or firmware in the repo or in messages:
   `tools/synth_nsp.py` makes disposable stand-ins.
6. When you stop or hand over, `send --to all --kind handoff -m "done / not done / files / what is next"`.

Mail is read only when an agent looks; nothing is pushed. Look at the start of every task and before you commit.

## Where things are
- `src/host/`: CPU (dynarmic), loader, kernel SVCs, memory, HLE stubs. `src/gfx/nvn/`: NVN layer (nxnative's `hle_nvn.c` ported) and its
  Vulkan backend. `src/gfx/vk/`: the Vulkan context. `tests/`: C/C++ tests run by CTest, guest programs in `tests/guest/`.
- Tests: `ctest` in your build directory; Python tests run with `python3 -I tests/<name>.py`.
- Who owns which area: the table in `docs/agents/README.md`.
