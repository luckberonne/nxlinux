# Working together in this checkout

Several agents work in this one working tree at the same time, built on different tools (Claude Code, Codex, Gemini, Cursor, Aider, ...),
together with the owner. The failure this protocol exists to prevent already happened once: one agent's `git add -A` committed another's
half-written file. One tool, `tools/agentmsg.py`, gives mail, claims and a safe commit. It is plain Python with no dependencies, and any
agent that can run a shell command can use it, whatever tool it is. Its state lives in `.agents/`, which git ignores.

## Names
`NAME` is a short, stable identifier for you (`codex`, `gemini`, `cursor`, a session name, ...), given with `--as NAME` or the `AGENT_NAME`
environment variable. Use the same one every time: mail and claims are filed under it. `all` addresses everyone. Say who you are once:
`python3 tools/agentmsg.py --as NAME send --to all -m "I am NAME, working on ..."`.

## The rules
1. **Start with** `python3 tools/agentmsg.py --as NAME start`: the rules in short, who claims what, your unread mail. Run it at the beginning of
   every task and before you commit. Programs can add `--json`.
2. **Claim what you are about to change** if it is outside your area (table below), or a large piece of your own:
   `python3 tools/agentmsg.py --as NAME claim PATH... --note "why"`. A conflicting claim is refused and says whose it is; ask them with
   `send`. A claim nobody refreshed for 12 hours is stale and stops blocking anyone. `release` when you are done.
3. **Commit only with** `python3 tools/agentmsg.py --as NAME commit -m "one line" PATH...`. It commits exactly those paths, refuses paths
   another agent has claimed, and rejects a message that is not one plain line or that mentions an agent or an AI (the owner's rule).
   Never `git add -A`, `git add .` or `git commit -a` here: they take other agents' work along with yours.
4. **Use your own build directory**, `build-NAME` (`cmake -S . -B build-NAME -G Ninja`). Two agents running CMake in one build directory
   corrupt each other's objects. `build/` belongs to nobody.
5. **Say what you leave behind.** When you stop or hand something over, `send --kind handoff` with: what is done, what is not, which files,
   what you were about to do. Ask a specific agent with `send --kind question --to THEM`; answer with `--reply-to ID`.
6. **If your tool can message the other agent live**, do that too, and still leave the file message: it is the record, and it is what a
   session started later will find.
7. **No game material, keys or firmware** in the repository, in messages or in commits. `tools/synth_nsp.py` makes disposable stand-ins.

Mail is read only when an agent looks; there is no push. Agents that load instruction files (`AGENTS.md` at the root, and `CLAUDE.md`, which
points to it) learn these rules on their own; an agent that was already running before this existed, or whose tool reads neither file, has to
be told once: the owner can paste `docs/agents/ONBOARD.txt` to it.

## Who owns what (durable; the claims in `.agents/` are the live view)
| Area | Owner |
|---|---|
| `src/`, `third_party/`, `CMakeLists.txt`, `games/`, C/C++ tests, `tests/guest/`, `tests/shaders/`, `tools/vkprobe.c`, `tools/nso.py`, `tools/gen_nvn_names.py`, `docs/nvn-port-analysis.md`, `docs/vulkan-notes.md`, `docs/phase0-findings.md` | `port-nxnative-linux-x86` (CPU emulation, host, NVN on Vulkan) |
| `tools/synth_nsp.py`, `tests/test_synth_nsp.py`, the synthetic-NSP notes in `docs/plan.md` | `synth-nsp` (disposable keys and a fake game for `nx_extract`) |
| `tools/agentmsg.py`, `tests/test_agentmsg.py`, `docs/agents/`, `AGENTS.md`, `CLAUDE.md` | shared; change them by message first |
| `docs/plan.md` (the rest), `README.md`, `.gitignore` | shared: add your own lines, message before restructuring |

If you are a new agent, add a row for your area (by message to `all` first) instead of editing someone else's.

## Quick reference
```
python3 tools/agentmsg.py --as NAME start [--json]
python3 tools/agentmsg.py --as NAME inbox [--peek] [--json]
python3 tools/agentmsg.py --as NAME send --to OTHER|all [--kind info|question|request|handoff] [--reply-to ID] -m "text"
python3 tools/agentmsg.py --as NAME claim PATH... [--note "why"]     /  release [PATH...]
python3 tools/agentmsg.py status [--json]
python3 tools/agentmsg.py --as NAME commit -m "one line" PATH...
```

## Optional: a pre-commit warning
`tools/agentmsg.py hook` prints staged files that another agent has claimed and who has unread mail; it warns and never blocks unless
`AGENTS_STRICT=1`. It would catch an agent that ignores every instruction file, but wiring it into git changes the repository's git
configuration (`core.hooksPath`), so it is not done here: the owner can add it if wanted.
