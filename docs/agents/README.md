# Working together in this checkout

Several agents (Claude sessions, and the owner) work in this one working tree at the same time. The failure this protocol exists to
prevent already happened once: one agent's `git add -A` committed another's half-written file. One tool, `tools/agentmsg.py`
(standard library only; its state lives in `.agents/`, which git ignores), gives mail, claims and a safe commit.

## Names
Use your session name (the one `ListAgents` shows for you, and that other sessions use with `SendMessage`) as `NAME`, or set
`AGENT_NAME` once. `all` addresses everyone.

## The rules
1. **Start by reading mail and claims.**
   `python3 tools/agentmsg.py --as NAME inbox` and `python3 tools/agentmsg.py status`.
2. **Claim what you are about to change** if it is outside your area (table below), or a large piece of your own:
   `python3 tools/agentmsg.py --as NAME claim PATH... --note "why"`. A claim that conflicts is refused and says whose it is; ask them with
   `send`. A claim nobody refreshed for 12 hours is stale and no longer blocks anyone. `release` when you are done.
3. **Commit only with** `python3 tools/agentmsg.py --as NAME commit -m "one line" PATH...`. It commits exactly those paths, refuses paths
   another agent has claimed, and rejects a message that is not one plain line or that mentions an agent or an AI (the owner's rule).
   Never `git add -A`, `git commit -a` or `git add .` here: they take other agents' work along with yours.
4. **Use your own build directory**: `build-NAME` (`cmake -S . -B build-NAME -G Ninja`). Two agents running CMake in one build directory
   corrupt each other's objects. `build/` belongs to nobody.
5. **Say what you leave behind.** When you stop, or hand something over, `send --kind handoff` with: what is done, what is not, which files,
   what you were about to do. Questions to a specific agent: `send --kind question --to NAME`, answered with `--reply-to ID`.
6. **Live sessions are faster**: if the other agent is listed by `ListAgents`, also `SendMessage` it. Leave the file message as well: it is
   the record, and it is what a session started later will find.
7. **No game material, keys or firmware** in the repository, in messages or in commits. `tools/synth_nsp.py` makes disposable stand-ins.

Mail is only read when an agent looks. There is no push: `CLAUDE.md` at the root makes every new session in this directory read these rules,
and rule 1 makes it look. A session that was already running before this existed will not know until it is told (`SendMessage`, or the owner).

## Who owns what (durable; the claims in `.agents/` are the live view)
| Area | Owner (session name) |
|---|---|
| `src/`, `third_party/`, `CMakeLists.txt`, `games/`, C/C++ tests, `tests/guest/`, `tests/shaders/`, `tools/vkprobe.c`, `tools/nso.py`, `tools/gen_nvn_names.py`, `docs/nvn-port-analysis.md`, `docs/vulkan-notes.md`, `docs/phase0-findings.md` | `port-nxnative-linux-x86` (CPU emulation, host, NVN on Vulkan) |
| `tools/synth_nsp.py`, `tests/test_synth_nsp.py`, the synthetic-NSP notes in `docs/plan.md` | `synth-nsp` (disposable keys and a fake game for `nx_extract`) |
| `tools/agentmsg.py`, `tests/test_agentmsg.py`, `docs/agents/`, `CLAUDE.md` | shared; change them by message first |
| `docs/plan.md` (the rest), `README.md`, `.gitignore` | shared: add your own lines, message before restructuring |

## Quick reference
```
python3 tools/agentmsg.py --as NAME inbox [--peek]
python3 tools/agentmsg.py --as NAME send --to OTHER|all [--kind info|question|request|handoff] [--reply-to ID] -m "text"
python3 tools/agentmsg.py --as NAME claim PATH... [--note "why"]     /  release [PATH...]
python3 tools/agentmsg.py status
python3 tools/agentmsg.py --as NAME commit -m "one line" PATH...
```

## Optional: a pre-commit warning
`tools/agentmsg.py hook` prints staged files that another agent has claimed and who has unread mail; it warns and never blocks unless
`AGENTS_STRICT=1`. Wiring it into git means changing the repository's git configuration (`core.hooksPath`), so it is not done here: the
owner can add it if wanted.
