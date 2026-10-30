"""tools/agentmsg.py in a throwaway git repository: mail, claims, commits that touch only what they should, the hook."""
import json, os, subprocess, sys, tempfile, time, unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent / 'tools' / 'agentmsg.py'


class T(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        self.git('init', '-q', '-b', 'main'); self.git('config', 'user.name', 't'); self.git('config', 'user.email', 't@example.com')
        (self.root / 'a').mkdir(); (self.root / 'b').mkdir()
        for f in ('a/one.txt', 'a/two.txt', 'b/three.txt'): (self.root / f).write_text('x\n')
        self.git('add', '-A'); self.git('commit', '-qm', 'base')

    def tearDown(self): self.tmp.cleanup()

    def git(self, *a): return subprocess.run(['git', '-C', str(self.root)] + list(a), capture_output=True, text=True, check=True).stdout

    def run_tool(self, who, *a, ok=True, env=None):
        e = dict(os.environ); e.pop('AGENT_NAME', None); e.update(env or {})
        cmd = [sys.executable, str(TOOL), '--root', str(self.root)] + (['--as', who] if who else []) + list(a)
        r = subprocess.run(cmd, capture_output=True, text=True, env=e, cwd=self.root)
        if ok and r.returncode: self.fail(f'{a} failed: {r.stderr}')
        if not ok and not r.returncode: self.fail(f'{a} should have failed: {r.stdout}')
        return r

    # ---- mail
    def test_direct_and_broadcast_mail(self):
        self.run_tool('alice', 'send', '--to', 'bob', '-m', 'hello bob', '--kind', 'question')
        self.run_tool('alice', 'send', '--to', 'all', '-m', 'to everyone')
        out = self.run_tool('bob', 'inbox').stdout
        self.assertIn('hello bob', out); self.assertIn('to everyone', out); self.assertIn('[question]', out)
        self.assertIn('no unread', self.run_tool('bob', 'inbox').stdout)                  # marked read
        self.assertIn('no unread', self.run_tool('alice', 'inbox').stdout)                # alice does not get her own broadcast
        out = self.run_tool('carol', 'inbox').stdout
        self.assertIn('to everyone', out); self.assertNotIn('hello bob', out)             # a direct message is for bob only

    def test_peek_does_not_mark_and_replies_link(self):
        mid = self.run_tool('alice', 'send', '--to', 'bob', '-m', 'q?').stdout.strip()
        self.assertIn('q?', self.run_tool('bob', 'inbox', '--peek').stdout)
        self.assertIn('q?', self.run_tool('bob', 'inbox', '--peek').stdout)
        self.run_tool('bob', 'send', '--to', 'alice', '--reply-to', mid, '-m', 'yes')
        out = self.run_tool('alice', 'inbox').stdout
        self.assertIn('yes', out); self.assertIn('re ' + mid, out)

    def test_identity_required_and_validated(self):
        self.run_tool(None, 'inbox', ok=False)
        self.run_tool('bad name', 'inbox', ok=False)
        self.run_tool('all', 'inbox', ok=False)
        self.assertIn('no unread', self.run_tool(None, 'inbox', env={'AGENT_NAME': 'dave'}).stdout)    # the environment works too
        self.run_tool('alice', 'send', '--to', '../x', '-m', 'y', ok=False)                    # no path tricks through the recipient

    # ---- claims
    def test_claims_conflict_by_prefix_and_release(self):
        self.run_tool('alice', 'claim', 'a', '--note', 'working on a')
        r = self.run_tool('bob', 'claim', 'a/one.txt', ok=False); self.assertIn('claimed by alice', r.stderr)    # inside her directory
        self.run_tool('bob', 'claim', 'b')                                                     # elsewhere is fine
        self.run_tool('bob', 'claim', 'a/two.txt', '--force')
        self.assertIn('alice', self.run_tool(None, 'status').stdout)
        self.run_tool('alice', 'release', 'a')
        self.run_tool('carol', 'claim', 'a/one.txt')                                          # free now
        self.run_tool('alice', 'release'); self.assertIn('nothing claimed', self.run_tool('alice', 'release').stdout)

    def test_stale_claim_does_not_block(self):
        self.run_tool('alice', 'claim', 'a')
        f = self.root / '.agents' / 'claims' / 'alice.json'
        old = time.time() - 13 * 3600; os.utime(f, (old, old))
        self.run_tool('bob', 'claim', 'a/one.txt')
        self.assertIn('stale', self.run_tool(None, 'status').stdout)

    # ---- commits
    def test_commit_takes_only_its_paths(self):
        (self.root / 'a/one.txt').write_text('mine\n'); (self.root / 'b/three.txt').write_text('theirs\n'); (self.root / 'b/new.txt').write_text('new\n')
        self.run_tool('alice', 'commit', '-m', 'Change a', 'a/one.txt')
        self.assertEqual(self.git('show', '--name-only', '--format=', 'HEAD').split(), ['a/one.txt'])
        self.assertIn('b/three.txt', self.git('status', '--porcelain'))                         # the other agent's work is still uncommitted
        self.assertIn('b/new.txt', self.git('status', '--porcelain'))
        self.run_tool('alice', 'commit', '-m', 'Add a new file', 'b/new.txt')
        self.assertEqual(self.git('show', '--name-only', '--format=', 'HEAD').split(), ['b/new.txt'])

    def test_commit_refuses_claimed_paths_and_bad_messages(self):
        (self.root / 'a/one.txt').write_text('changed\n')
        self.run_tool('bob', 'claim', 'a')
        r = self.run_tool('alice', 'commit', '-m', 'Change', 'a/one.txt', ok=False); self.assertIn('claimed by bob', r.stderr)
        self.assertEqual(self.git('log', '--oneline').count('\n'), 1)                           # nothing was committed
        for bad in ('Co-Authored-By: someone', 'generated with a tool', 'Fix by Claude', 'two\nlines', ''):
            self.run_tool('bob', 'commit', '-m', bad, 'a/one.txt', ok=False)
        self.run_tool('bob', 'commit', '-m', 'Change a', 'a/one.txt')                            # his own claim, a clean message: fine
        self.assertEqual(self.git('log', '--format=%s', '-1').strip(), 'Change a')

    def test_commit_does_not_claim_what_it_commits(self):
        (self.root / 'a/one.txt').write_text('1\n'); (self.root / 'a/two.txt').write_text('2\n')
        self.run_tool('alice', 'commit', '-m', 'Change one', 'a/one.txt')                  # no claim of her own
        self.assertNotIn('a/one.txt', self.run_tool(None, 'status').stdout)
        self.run_tool('bob', 'commit', '-m', 'Change two', 'a/two.txt')                    # bob can touch the neighbour, and the same file again
        (self.root / 'a/one.txt').write_text('1b\n'); self.run_tool('bob', 'commit', '-m', 'Change one again', 'a/one.txt')
        self.run_tool('alice', 'claim', 'a/two.txt'); f = self.root / '.agents' / 'claims' / 'alice.json'
        old = time.time() - 5 * 3600; os.utime(f, (old, old))
        (self.root / 'a/two.txt').write_text('2b\n'); self.run_tool('alice', 'commit', '-m', 'Change two again', 'a/two.txt')
        self.assertLess(time.time() - f.stat().st_mtime, 60)                               # committing under a claim keeps it fresh

    def test_commit_handles_deletions(self):
        (self.root / 'a/two.txt').unlink()
        self.run_tool('alice', 'commit', '-m', 'Remove two', 'a/two.txt')
        self.assertFalse((self.root / 'a/two.txt').exists())
        self.assertIn('D\ta/two.txt', self.git('show', '--name-status', '--format=', 'HEAD'))

    def test_commit_untracks_an_ignored_file_and_leaves_other_staged_work(self):
        (self.root / '.gitignore').write_text('*.pyc\n'); self.git('add', '.gitignore'); self.git('commit', '-qm', 'ignore pyc')
        (self.root / 'a/x.pyc').write_bytes(b'1'); self.git('add', '-f', 'a/x.pyc'); self.git('commit', '-qm', 'tracked by mistake')
        (self.root / 'a/x.pyc').write_bytes(b'2')                                                   # modified on disk, as a rebuilt cache is
        self.git('rm', '--cached', '-q', 'a/x.pyc')
        (self.root / 'b/three.txt').write_text('staged by bob\n'); self.git('add', 'b/three.txt')
        self.run_tool('alice', 'commit', '-m', 'Stop tracking pyc', 'a/x.pyc')
        self.assertEqual(self.git('show', '--name-status', '--format=', 'HEAD').strip(), 'D\ta/x.pyc')   # removed, not re-added modified
        self.assertTrue((self.root / 'a/x.pyc').exists())
        self.assertEqual(self.git('diff', '--cached', '--name-only').strip(), 'b/three.txt')         # bob's staged file is still staged, untouched

    # ---- the entry point for an agent of any kind, and JSON for programs
    def test_start_shows_rules_status_and_mail_once(self):
        self.run_tool('alice', 'claim', 'a', '--note', 'on a')
        self.run_tool('alice', 'send', '--to', 'bob', '--kind', 'question', '-m', 'are you there?')
        out = self.run_tool('bob', 'start').stdout
        for needle in ('You are bob', 'commit -m', 'never mention an agent', 'alice', 'on a', 'are you there?'): self.assertIn(needle, out)
        self.assertIn('no unread', self.run_tool('bob', 'start').stdout)                 # read once, not shown again

    def test_json_output_parses(self):
        self.run_tool('alice', 'claim', 'a')
        self.run_tool('alice', 'send', '--to', 'bob', '-m', 'hi', '--kind', 'request')
        st = json.loads(self.run_tool(None, 'status', '--json').stdout)
        self.assertEqual(st['claims'][0]['agent'], 'alice'); self.assertEqual(st['claims'][0]['paths'], ['a']); self.assertEqual(st['unread'], {'bob': 1})
        peek = json.loads(self.run_tool('bob', 'inbox', '--json', '--peek').stdout)
        self.assertEqual([m['text'] for m in peek], ['hi']); self.assertEqual(peek[0]['kind'], 'request')
        both = json.loads(self.run_tool('bob', 'start', '--json').stdout)                # one document, not several
        self.assertEqual(set(both), {'you', 'status', 'inbox'}); self.assertEqual(both['you'], 'bob'); self.assertEqual(len(both['inbox']), 1)
        self.assertEqual(json.loads(self.run_tool('bob', 'inbox', '--json').stdout), [])

    # ---- hook
    def test_hook_warns_and_strict_blocks(self):
        self.run_tool('bob', 'claim', 'a')
        (self.root / 'a/one.txt').write_text('edited\n'); self.git('add', 'a/one.txt')
        self.run_tool('alice', 'send', '--to', 'bob', '-m', 'hi')
        r = self.run_tool('alice', 'hook'); self.assertIn('a/one.txt is claimed by bob', r.stderr); self.assertIn('bob: 1', r.stderr)
        self.run_tool('alice', 'hook', ok=False, env={'AGENTS_STRICT': '1'})
        self.assertNotIn('claimed by', self.run_tool('bob', 'hook').stderr)                      # his own claim is no warning


if __name__ == '__main__':
    unittest.main()
