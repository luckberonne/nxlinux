#!/usr/bin/env python3
"""Coordination between agents (Claude sessions, people) that work in the same checkout.

State lives in <repo>/.agents/ (git-ignored): one JSON file per message, one claims file per agent. Standard library only.

  agentmsg.py --as NAME send --to NAME|all [--kind info|question|request|handoff] [--reply-to ID] -m TEXT
  agentmsg.py --as NAME start                 the one command to begin with: the rules in short, who claims what, your unread mail
  agentmsg.py --as NAME inbox [--peek]        unread messages for NAME and for "all"; marks them read unless --peek
  agentmsg.py --as NAME claim PATH... [--note TEXT] [--force]
  agentmsg.py --as NAME release [PATH...]     without paths, releases everything
  agentmsg.py status                          who claims what, and who has unread mail
  agentmsg.py --as NAME commit -m "one line" PATH...   commits only those paths, refusing paths another agent has claimed
  agentmsg.py hook                            what .githooks/pre-commit runs: warns, never blocks (AGENTS_STRICT=1 blocks)

NAME comes from --as or the AGENT_NAME environment variable. See docs/agents/README.md. Any agent that can run a shell command can use it,
whatever tool it is; --json makes inbox, status and start print JSON for programs.
"""
import argparse, json, os, re, secrets, subprocess, sys, time
from datetime import datetime, timezone
from pathlib import Path

STALE_HOURS = 12            # a claim not refreshed for this long no longer blocks anyone
NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')
FORBIDDEN_IN_COMMIT = re.compile(r'claude|anthropic|\bAI\b|co-authored|generated with|🤖', re.I)   # the owner's rule: commits never mention an agent


def root_dir(args):
    if args.root: return Path(args.root).resolve()
    r = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
    if r.returncode: sys.exit('agentmsg: not inside a git repository (use --root)')
    return Path(r.stdout.strip())


def now_iso(): return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%fZ')[:-4] + 'Z'


def need_name(args):
    name = args.agent or os.environ.get('AGENT_NAME')
    if not name: sys.exit('agentmsg: who are you? pass --as NAME or set AGENT_NAME')
    if not NAME_RE.match(name) or name == 'all': sys.exit(f'agentmsg: bad agent name {name!r}')
    return name


def atomic_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f'.{path.name}.{os.getpid()}.tmp')
    tmp.write_text(text, encoding='utf-8'); os.replace(tmp, path)


# ---------------------------------------------------------------- mail
def mail_dir(st, to): return st / 'mail' / to
def read_file(st, name): return st / 'read' / f'{name}.txt'
def read_ids(st, name):
    f = read_file(st, name)
    return set(f.read_text().split()) if f.exists() else set()


def load_mail(st, who):
    out = []
    for box in (who, 'all'):
        d = mail_dir(st, box)
        if not d.is_dir(): continue
        for f in sorted(d.glob('*.json')):
            try: m = json.loads(f.read_text(encoding='utf-8'))
            except (OSError, ValueError): continue
            if m.get('from') != who: out.append(m)
    return sorted(out, key=lambda m: m['id'])


def cmd_send(st, args):
    me = need_name(args)
    to = args.to
    if to != 'all' and not NAME_RE.match(to): sys.exit(f'agentmsg: bad recipient {to!r}')
    text = args.message if args.message is not None else sys.stdin.read()
    if not text.strip(): sys.exit('agentmsg: empty message')
    mid = f"{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S')}-{me}-{secrets.token_hex(2)}"
    atomic_write(mail_dir(st, to) / f'{mid}.json',
                 json.dumps({'id': mid, 'from': me, 'to': to, 'kind': args.kind, 'ts': now_iso(), 'reply_to': args.reply_to, 'text': text.rstrip()}, indent=1) + '\n')
    print(mid)


def unread_for(st, me):
    seen = read_ids(st, me)
    return [m for m in load_mail(st, me) if m['id'] not in seen]


def mark_read(st, me, msgs):
    f = read_file(st, me); f.parent.mkdir(parents=True, exist_ok=True)
    with open(f, 'a') as fh: fh.write(''.join(m['id'] + '\n' for m in msgs))


def print_messages(msgs):
    if not msgs: print('no unread messages'); return
    for m in msgs:
        print(f"--- {m['id']}  from {m['from']}  to {m['to']}  [{m['kind']}]" + (f"  re {m['reply_to']}" if m.get('reply_to') else ''))
        print(m['text'])


def cmd_inbox(st, args):
    me = need_name(args)
    unread = unread_for(st, me)
    print(json.dumps(unread, indent=1)) if args.json else print_messages(unread)
    if unread and not args.peek: mark_read(st, me, unread)


# ---------------------------------------------------------------- claims
def norm(root, p):
    q = Path(p)
    q = (q if q.is_absolute() else Path.cwd() / q).resolve()
    try: return q.relative_to(root).as_posix()
    except ValueError: sys.exit(f'agentmsg: {p} is outside the repository')


def overlap(a, b):
    return a == b or a.startswith(b.rstrip('/') + '/') or b.startswith(a.rstrip('/') + '/')


def load_claims(st, skip=None):
    out = []
    d = st / 'claims'
    if not d.is_dir(): return out
    for f in d.glob('*.json'):
        try: c = json.loads(f.read_text(encoding='utf-8'))
        except (OSError, ValueError): continue
        if c.get('agent') == skip: continue
        age_h = (time.time() - f.stat().st_mtime) / 3600
        c['stale'] = age_h > STALE_HOURS; c['age_h'] = age_h
        out.append(c)
    return out


def conflicts(st, me, paths):
    hits = []
    for c in load_claims(st, skip=me):
        if c['stale']: continue
        for p in paths:
            for q in c['paths']:
                if overlap(p, q): hits.append((p, c['agent'], q, c.get('note', '')))
    return hits


def save_claim(st, me, paths, note):
    f = st / 'claims' / f'{me}.json'
    old = json.loads(f.read_text()) if f.exists() else {'agent': me, 'since': now_iso(), 'paths': [], 'note': ''}
    old['paths'] = sorted(set(old['paths']) | set(paths)); old['updated'] = now_iso()
    if note: old['note'] = note
    atomic_write(f, json.dumps(old, indent=1) + '\n')


def cmd_claim(st, root, args):
    me = need_name(args)
    paths = [norm(root, p) for p in args.paths]
    hits = conflicts(st, me, paths)
    if hits and not args.force:
        for p, who, q, note in hits: print(f'{p}: claimed by {who} ({q}){": " + note if note else ""}', file=sys.stderr)
        sys.exit('agentmsg: not claimed (talk to them with `send`, or --force)')
    save_claim(st, me, paths, args.note)
    print('claimed: ' + ' '.join(paths))


def cmd_release(st, root, args):
    me = need_name(args)
    f = st / 'claims' / f'{me}.json'
    if not f.exists(): print('nothing claimed'); return
    if not args.paths: f.unlink(); print('released everything'); return
    c = json.loads(f.read_text()); drop = [norm(root, p) for p in args.paths]
    c['paths'] = [p for p in c['paths'] if not any(overlap(p, d) for d in drop)]
    if c['paths']: atomic_write(f, json.dumps(c, indent=1) + '\n')
    else: f.unlink()
    print('released: ' + ' '.join(drop))


def unread_counts(st):
    out = {}
    mail = st / 'mail'
    if mail.is_dir():
        for box in sorted(p.name for p in mail.iterdir() if p.is_dir() and p.name != 'all'):
            n = len(unread_for(st, box))
            if n: out[box] = n
    return out


def status_data(st):
    claims = [{k: c.get(k) for k in ('agent', 'paths', 'note', 'since', 'updated', 'stale', 'age_h')} for c in sorted(load_claims(st), key=lambda c: c['agent'])]
    return {'claims': claims, 'unread': unread_counts(st)}


def print_status(d):
    if not d['claims']: print('no claims')
    for c in d['claims']:
        print(f"{c['agent']}{'  (stale: not refreshed for %.0f h)' % c['age_h'] if c['stale'] else ''}" + (f"  - {c['note']}" if c.get('note') else ''))
        for p in c['paths']: print('   ' + p)
    for box, n in d['unread'].items(): print(f'{box}: {n} unread message(s)')


def cmd_status(st, args):
    d = status_data(st)
    print(json.dumps(d, indent=1)) if args.json else print_status(d)


RULES = """Rules for this shared checkout (full text: docs/agents/README.md, AGENTS.md)
 1. Read your mail first (this command did). Ask the other agent with: send --to NAME --kind question -m "..."
 2. Claim what you will change outside your own area:  claim PATH... --note "why"   (release when done)
 3. Commit ONLY with:  commit -m "one plain line" PATH...    Never git add -A / git commit -a / git add .
 4. Build in your own directory (build-NAME), not build/.
 5. When you stop or hand over:  send --to all --kind handoff -m "done / not done / files / next"
 6. No game files, keys or firmware in the repo or in messages. Commit messages never mention an agent or an AI."""


def cmd_start(st, root, args):
    me = need_name(args)
    unread = unread_for(st, me)
    if args.json:
        print(json.dumps({'you': me, 'status': status_data(st), 'inbox': unread}, indent=1))
    else:
        print(f'You are {me}.\n\n{RULES}\n\n== status =='); print_status(status_data(st))
        print('\n== your mail =='); print_messages(unread)
    if unread: mark_read(st, me, unread)


# ---------------------------------------------------------------- git
def cmd_commit(st, root, args):
    me = need_name(args)
    msg = args.message
    if '\n' in msg.strip() or not msg.strip(): sys.exit('agentmsg: the commit message is one line')
    if FORBIDDEN_IN_COMMIT.search(msg): sys.exit('agentmsg: the commit message must not mention an agent or an AI')
    paths = [norm(root, p) for p in args.paths]
    hits = conflicts(st, me, paths)
    if hits:
        for p, who, q, note in hits: print(f'{p}: claimed by {who} ({q})', file=sys.stderr)
        sys.exit('agentmsg: refusing to commit paths another agent has claimed')
    save_claim(st, me, paths, '')                      # committing refreshes (and takes) the claim on what it touches
    git = ['git', '-C', str(root)]
    subprocess.run(git + ['add', '-A', '--'] + paths, check=True)
    r = subprocess.run(git + ['commit', '-m', msg, '--'] + paths, capture_output=True, text=True)
    sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
    if r.returncode: sys.exit(r.returncode)
    print(subprocess.run(git + ['rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip())


def cmd_hook(st, root, args):
    staged = subprocess.run(['git', '-C', str(root), 'diff', '--cached', '--name-only'], capture_output=True, text=True).stdout.split()
    me = args.agent or os.environ.get('AGENT_NAME')
    warn = []
    for c in load_claims(st, skip=me):
        if c['stale']: continue
        for p in staged:
            if any(overlap(p, q) for q in c['paths']): warn.append(f"  {p} is claimed by {c['agent']}")
    unread = [f'{box}: {n}' for box, n in unread_counts(st).items()]
    if warn: print('agentmsg: staged files another agent has claimed:\n' + '\n'.join(warn), file=sys.stderr)
    if unread: print('agentmsg: unread messages -> ' + ', '.join(unread) + '  (tools/agentmsg.py --as NAME inbox)', file=sys.stderr)
    if warn and os.environ.get('AGENTS_STRICT') == '1': sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--as', dest='agent'); ap.add_argument('--root'); ap.add_argument('--json', action='store_true', help='machine-readable output for inbox, status and start')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('send'); s.add_argument('--to', required=True); s.add_argument('--kind', default='info', choices=['info', 'question', 'request', 'handoff'])
    s.add_argument('--reply-to'); s.add_argument('-m', '--message')
    i = sub.add_parser('inbox'); i.add_argument('--peek', action='store_true')
    st_p = sub.add_parser('status'); sa = sub.add_parser('start')
    for sp in (i, st_p, sa): sp.add_argument('--json', action='store_true', default=argparse.SUPPRESS, help='JSON output (also accepted before the subcommand)')
    c = sub.add_parser('claim'); c.add_argument('paths', nargs='+'); c.add_argument('--note', default=''); c.add_argument('--force', action='store_true')
    r = sub.add_parser('release'); r.add_argument('paths', nargs='*')
    g = sub.add_parser('commit'); g.add_argument('-m', '--message', required=True); g.add_argument('paths', nargs='+')
    sub.add_parser('hook')
    args = ap.parse_args()
    root = root_dir(args); st = root / '.agents'
    {'send': lambda: cmd_send(st, args), 'inbox': lambda: cmd_inbox(st, args), 'claim': lambda: cmd_claim(st, root, args),
     'release': lambda: cmd_release(st, root, args), 'status': lambda: cmd_status(st, args), 'commit': lambda: cmd_commit(st, root, args),
     'hook': lambda: cmd_hook(st, root, args), 'start': lambda: cmd_start(st, root, args)}[args.cmd]()


if __name__ == '__main__':
    main()
