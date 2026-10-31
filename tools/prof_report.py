#!/usr/bin/env python3
"""Turns the file NX_PROF writes (kill -USR2 <pid>) into tables: where the samples are by thread, by module and by function of the host binary, and which callers lead to the hottest functions.

  prof_report.py nx_prof.txt [path/to/nxlinux] [--top N] [--stack FUNC] [--log RUN.log]

With --log (the log of the same run, which names where each guest module was loaded) the program counters of the guest threads are listed too: the hottest 64-byte blocks of each module.

Samples at addresses outside the binary are named by the module that maps them; anonymous executable memory is the guest's JIT-compiled code (dynarmic)."""
import bisect, collections, re, subprocess, sys
from pathlib import Path


def main(argv):
    logf = argv[argv.index('--log') + 1] if '--log' in argv else None
    args = [a for a in argv[1:] if not a.startswith('--') and a != logf and not (a.isdigit() and argv[argv.index(a) - 1] == '--top') and a != (argv[argv.index('--stack') + 1] if '--stack' in argv else None)]
    top = int(argv[argv.index('--top') + 1]) if '--top' in argv else 25
    stack = argv[argv.index('--stack') + 1] if '--stack' in argv else None
    if not args: sys.exit(__doc__)
    exe = args[1] if len(args) > 1 else 'build-claude/nxlinux'
    base = 0; maps = []; comm = {}; samples = []; guest = []
    for ln in Path(args[0]).read_text().splitlines():
        w = ln.split(None, 4)
        if ln.startswith('base '): base = int(w[1], 16)
        elif ln.startswith('M '): maps.append((int(w[1], 16), int(w[2], 16), w[4].strip() if len(w) > 4 else 'anon', int(w[3], 16)))
        elif ln.startswith('T '): comm[int(w[1])] = w[2] if len(w) > 2 else ''
        elif ln.startswith('S '): samples.append((int(w[1]), [int(x, 16) for x in ln.split()[2:]]))
        elif ln.startswith('G '): guest.append((int(w[1]), int(w[2], 16), int(w[3], 16)))
    syms = []
    out = subprocess.run(['nm', '-n', '-C', '--defined-only', exe], capture_output=True, text=True).stdout
    for ln in out.splitlines():
        m = re.match(r'([0-9a-f]+) \w (.+)', ln)
        if m: syms.append((int(m.group(1), 16), m.group(2)))
    addrs = [a for a, _ in syms]

    libsyms = {}
    def lib_name(mod, a):               # a function of a shared library, from its exported symbols; the address is turned into a file offset through the mapping that holds it
        if mod not in libsyms:
            try: o = subprocess.run(['nm', '-D', '-n', '--defined-only', mod], capture_output=True, text=True).stdout
            except Exception: o = ''
            L = []
            for ln in o.splitlines():
                m = re.match(r'([0-9a-f]+) \w (\S+)', ln)
                if m: L.append((int(m.group(1), 16), m.group(2)))
            libsyms[mod] = (L, [x for x, _ in L])
        L, A = libsyms[mod]
        off = next((a - lo + o for lo, hi, nm, o in maps if lo <= a < hi), None)
        if not L or off is None: return Path(mod).name
        i = bisect.bisect_right(A, off) - 1
        return (L[i][1].split('@')[0] if i >= 0 else '?') + ' (' + Path(mod).name + ')'

    def module(a):
        for lo, hi, nm, o in maps:
            if lo <= a < hi: return nm
        return 'unmapped'

    def name(a):
        mod = module(a)
        if mod.endswith(Path(exe).name) or mod == str(Path(exe).resolve()):
            i = bisect.bisect_right(addrs, a - base) - 1
            return syms[i][1] if i >= 0 else '?'
        if mod in ('anon', '[heap]') or mod.startswith('['): return 'JIT/anonymous code'
        return lib_name(mod, a)

    n = len(samples)
    print(f'{n} samples\n')
    bt = collections.Counter(t for t, _ in samples)
    print('by thread (samples, share, name):')
    for t, c in bt.most_common(8): print(f'  {t} {comm.get(t, "?")}: {c} ({100 * c / n:.1f}%)')
    print('\nby module of the program counter:')
    bm = collections.Counter(Path(module(f[0])).name if not module(f[0]).startswith('[') and module(f[0]) != 'anon' else 'JIT/anonymous code' for _, f in samples)
    for k, c in bm.most_common(10): print(f'  {k}: {c} ({100 * c / n:.1f}%)')
    print(f'\nthe {top} hottest functions (program counter):')
    bf = collections.Counter(name(f[0]) for _, f in samples)
    for k, c in bf.most_common(top): print(f'  {100 * c / n:5.1f}%  {c:6d}  {k[:110]}')
    print(f'\nthe {top} functions that are on the stack of most samples (inclusive):')
    inc = collections.Counter()
    for _, f in samples:
        for k in set(name(a) for a in f): inc[k] += 1
    for k, c in inc.most_common(top): print(f'  {100 * c / n:5.1f}%  {c:6d}  {k[:110]}')
    print(f'\nsamples attributed to the first function of the host binary on the stack (self plus what it calls in libc, the driver and the JIT):')
    exe_name = Path(exe).name
    att = collections.Counter()
    for _, f in samples:
        who = next((name(a) for a in f if module(a).endswith(exe_name)), '(no host function: ' + name(f[0])[:40] + ')')
        att[who] += 1
    for k, c in att.most_common(top): print(f'  {100 * c / n:5.1f}%  {c:6d}  {k[:110]}')
    if stack:
        print(f'\ncallers of functions matching "{stack}":')
        cc = collections.Counter()
        for _, f in samples:
            names = [name(a) for a in f]
            for i, k in enumerate(names):
                if stack in k: cc[' <- '.join(x[:48] for x in names[i:i + 4])] += 1; break
        for k, c in cc.most_common(10): print(f'  {c:5d}  {k}')
    if guest:
        mods = []
        if logf:
            for ln in Path(logf).read_text().splitlines():
                m = re.match(r'\[dm\] module (\S+)\s+base (0x[0-9a-f]+) size (0x[0-9a-f]+)', ln)
                if m: mods.append((int(m.group(2), 16), int(m.group(2), 16) + int(m.group(3), 16), m.group(1)))
        def gname(a):
            for lo, hi, nm in mods:
                if lo <= a < hi: return f'{nm}+{a - lo:#x}'
            return f'{a:#x}'
        ng = len(guest)
        print(f'\n{ng} guest samples; by thread:')
        for t, c in collections.Counter(t for t, _, _ in guest).most_common(6): print(f'  {t} {comm.get(t, "?")}: {c} ({100 * c / ng:.1f}%)')
        print(f'\nthe {top} hottest 64-byte blocks of guest code (module + offset):')
        blk = collections.Counter(gname(pc & ~63) for _, pc, _ in guest)
        for k, c in blk.most_common(top):
            callers = collections.Counter(gname(lr) for _, pc, lr in guest if gname(pc & ~63) == k).most_common(2)
            print(f'  {100 * c / ng:5.1f}%  {c:6d}  {k}   called from {", ".join(a for a, _ in callers)}')
        print('\nby module:')
        for k, c in collections.Counter(gname(pc).split('+')[0] for _, pc, _ in guest).most_common(6): print(f'  {k}: {c} ({100 * c / ng:.1f}%)')


main(sys.argv)
