#!/usr/bin/env python3
"""Assemble the directory tools/shader_pack.py packs into a shader library for a Switch 2 (Ounce) title.

  ounce_pack_dir.py OUT_DIR RUNTIME_DIR ARCHIVE_DIR [STANDIN_DIR]

RUNTIME_DIR holds what the title builds itself (NX_SHADER_DUMP=<dir> NX_SHADER_DUMP_HASHED=1, then ounce_sass.py and ounce_xlate on it): programs named by the first 16 hex digits of the SHA-256
of their code area. ARCHIVE_DIR holds the programs of the title's shader archives (ounce_extract.py output, named by the control hash, which names several builds): each is re-keyed by the same
SHA-256 of its code. OUT_DIR gets symlinks <key>.spv, <key>.res.json, <key>.c1 and (when there is one) <key>.ctl for every program that translated; a run-time build wins over an archive build of the same code."""
import hashlib, os, sys
from pathlib import Path

out, runtime, archive = (Path(a).resolve() for a in sys.argv[1:4])
standin = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else None
out.mkdir(parents=True, exist_ok=True)
n = {'runtime': 0, 'archive': 0}


def link(src, key):
    for ext in ('spv', 'res.json', 'c1', 'ctl'):
        s = src / f'{src_key}.{ext}'
        d = out / f'{key}.{ext}'
        if s.exists() and not d.exists(): d.symlink_to(s)


for kind, d in (('runtime', runtime), ('archive', archive)):
    for spv in sorted(d.glob('*.spv')):
        src_key = spv.stem
        code = d / f'{src_key}.code'
        key = src_key if kind == 'runtime' else hashlib.sha256(code.read_bytes()).hexdigest()[:16]
        if (out / f'{key}.spv').exists(): continue
        link(d, key); n[kind] += 1
if standin:      # hand-written programs (work/run/ndve_bypass: the bilinear stand-in for the video enhancement's last program, which renders noise when translated) replace the translated ones of the same key
    for f in sorted(standin.glob('*.spv')):
        for ext in ('spv', 'res.json', 'c1', 'ctl'):
            d = out / f'{f.stem}.{ext}'; s = standin / f'{f.stem}.{ext}'
            if d.is_symlink() or d.exists(): d.unlink()
            if s.exists(): d.symlink_to(s)
        print(f'stand-in for {f.stem}')
print(f"{n['runtime']} run-time programs, {n['archive']} archive programs -> {out}")
