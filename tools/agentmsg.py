#!/usr/bin/env python3
"""Coordination between agents (Claude sessions, people) that work in the same checkout.

State lives in <repo>/.agents/ (git-ignored): one JSON file per message, one claims file per agent. Standard library only.

  agentmsg.py --as NAME send --to NAME|all [--kind info|question|request|handoff] [--reply-to ID] -m TEXT
  agentmsg.py --as NAME inbox [--peek]        unread messages for NAME and for "all"; marks them read unless --peek
  agentmsg.py --as NAME claim PATH... [--note TEXT] [--force]
  agentmsg.py --as NAME release [PATH...]     without paths, releases everything
  agentmsg.py status                          who claims what, and who has unread mail
  agentmsg.py --as NAME commit -m "one line" PATH...   commits only those paths, refusing paths another agent has claimed
  agentmsg.py hook                            what .githooks/pre-commit runs: warns, never blocks (AGENTS_STRICT=1 blocks)

NAME comes from --as or the AGENT_NAME environment variable. See docs/agents/README.md.
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


def cmd_inbox(st, args):
    me = need_name(args)
    seen = read_ids(st, me)
    unread = [m for m in load_mail(st, me) if m['id'] not in seen]
    if not unread: print('no unread messages'); return
    for m in unread:
        print(f"--- {m['id']}  from {m['from']}  to {m['to']}  [{m['kind']}]" + (f"  re {m['reply_to']}" if m.get('reply_to') else ''))
        print(m['text'])
    if not args.peek:
        f = read_file(st, me); f.parent.mkdir(parents=True, exist_ok=True)
        with open(f, 'a') as fh: fh.write(''.join(m['id'] + '\n' for m in unread))


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


def cmd_status(st, args):
    claims = load_claims(st)
    if not claims: print('no claims')
    for c in sorted(claims, key=lambda c: c['agent']):
        print(f"{c['agent']}{'  (stale: not refreshed for %.0f h)' % c['age_h'] if c['stale'] else ''}" + (f"  - {c['note']}" if c.get('note') else ''))
        for p in c['paths']: print('   ' + p)
    mail = st / 'mail'
    if mail.is_dir():
        for box in sorted(p.name for p in mail.iterdir() if p.is_dir() and p.name != 'all'):
            n = len([m for m in load_mail(st, box) if m['id'] not in read_ids(st, box)])
            if n: print(f'{box}: {n} unread message(s)')


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
    mail = st / 'mail'
    unread = []
    if mail.is_dir():
        for box in sorted(p.name for p in mail.iterdir() if p.is_dir() and p.name != 'all'):
            n = len([m for m in load_mail(st, box) if m['id'] not in read_ids(st, box)])
            if n: unread.append(f'{box}: {n}')
    if warn: print('agentmsg: staged files another agent has claimed:\n' + '\n'.join(warn), file=sys.stderr)
    if unread: print('agentmsg: unread messages -> ' + ', '.join(unread) + '  (tools/agentmsg.py --as NAME inbox)', file=sys.stderr)
    if warn and os.environ.get('AGENTS_STRICT') == '1': sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--as', dest='agent'); ap.add_argument('--root')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('send'); s.add_argument('--to', required=True); s.add_argument('--kind', default='info', choices=['info', 'question', 'request', 'handoff'])
    s.add_argument('--reply-to'); s.add_argument('-m', '--message')
    i = sub.add_parser('inbox'); i.add_argument('--peek', action='store_true')
    c = sub.add_parser('claim'); c.add_argument('paths', nargs='+'); c.add_argument('--note', default=''); c.add_argument('--force', action='store_true')
    r = sub.add_parser('release'); r.add_argument('paths', nargs='*')
    sub.add_parser('status')
    g = sub.add_parser('commit'); g.add_argument('-m', '--message', required=True); g.add_argument('paths', nargs='+')
    sub.add_parser('hook')
    args = ap.parse_args()
    root = root_dir(args); st = root / '.agents'
    {'send': lambda: cmd_send(st, args), 'inbox': lambda: cmd_inbox(st, args), 'claim': lambda: cmd_claim(st, root, args),
     'release': lambda: cmd_release(st, root, args), 'status': lambda: cmd_status(st, args), 'commit': lambda: cmd_commit(st, root, args),
     'hook': lambda: cmd_hook(st, root, args)}[args.cmd]()


if __name__ == '__main__':
    main()
