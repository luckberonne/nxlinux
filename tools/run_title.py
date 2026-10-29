#!/usr/bin/env python3
"""Run a title: extract an NSP (and its update) with the vendored nx_extract, turn each ExeFS module into a memory image with tools/nso.py, write the manifest, start nxlinux.

  run_title.py --keys prod.keys --nsp game.nsp [--update upd.nsp] [--work DIR] [--entry SYMBOL] [--shaders LIB.nxsl] [--no-run]

The keys and the dumps come from your own console (Lockpick_RCM, nxdumptool) and stay wherever you keep them: nothing is copied into the repository, and the extracted files go
under --work (default work/run/<title>, which git ignores). Modules load in the order the Switch's loader does: rtld, main, subsdk0..9, sdk."""
import argparse, os, re, shutil, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'tools'))
import nso

ORDER = ['rtld', 'main'] + [f'subsdk{i}' for i in range(10)] + ['sdk']


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--keys', required=True); ap.add_argument('--nsp', required=True); ap.add_argument('--update')
    ap.add_argument('--work'); ap.add_argument('--entry'); ap.add_argument('--shaders'); ap.add_argument('--mem-mb', type=int)
    ap.add_argument('--nx-extract', default=str(REPO / 'work' / 'nx_extract' / 'nx_extract'))
    ap.add_argument('--exe', default=str(REPO / 'build' / 'nxlinux'))
    ap.add_argument('--no-run', action='store_true')
    a = ap.parse_args(argv)
    for what, p in (('keys', a.keys), ('NSP', a.nsp), ('nx_extract', a.nx_extract)):
        if not Path(p).is_file(): sys.exit(f'[RED] {what} not found: {p}')
    if not a.no_run and not Path(a.exe).is_file(): sys.exit(f'[RED] {a.exe} is not built (cmake --build build --target nxlinux)')
    work = Path(a.work) if a.work else REPO / 'work' / 'run' / re.sub(r'\W+', '_', Path(a.nsp).stem)
    out = work / 'extract'
    shutil.rmtree(out, ignore_errors=True); work.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([a.nx_extract, a.keys, a.nsp, a.update or '-', str(out)], capture_output=True, text=True)
    if p.returncode:
        sys.exit('[RED] nx_extract failed (are the keys the ones for this dump?):\n' + p.stdout + p.stderr)
    exefs = out / 'ExeFS'
    names = [n for n in ORDER if (exefs / n).is_file()]
    if not names: sys.exit(f'[RED] no code modules in {exefs}')
    lines = []
    for n in names:
        m = nso.cli_read(exefs / n)
        img = work / f'{n}.img'
        img.write_bytes(bytes(m.image))
        with open(img, 'r+b') as f: f.truncate(len(m.image) + m.bss)       # bss as a sparse zero tail: the host reads (file size - bss) bytes of image
        lines.append(f'{n} {img} {m.bss:x}')
        print(f'{n}: image {len(m.image):#x}, bss {m.bss:#x}, build id {m.build_id[:8].hex()}')
    manifest = work / 'modules.txt'
    manifest.write_text('\n'.join(lines) + '\n')
    cmd = [a.exe, '--manifest', str(manifest)]
    if a.entry: cmd += ['--entry', a.entry]
    if a.shaders: cmd += ['--shaders', a.shaders]
    if a.mem_mb: cmd += ['--mem-mb', str(a.mem_mb)]
    print('run:', ' '.join(cmd))
    if a.no_run: return 0
    return subprocess.run(cmd).returncode


if __name__ == '__main__':
    sys.exit(main())
