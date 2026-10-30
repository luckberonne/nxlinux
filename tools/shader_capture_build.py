#!/usr/bin/env python3
"""Turns the shaders a run captured (NX_SHADER_DUMP=<dir>, see hle_nvn.c shader_dump) into a shader library the host loads with --shaders.

  shader_capture_build.py DUMP_DIR OUT.nxsl [--work DIR] [--xlate DLL]

For every <key>.code in DUMP_DIR (the key is the one the host computes from the control section and the code): the stage comes from <key>.stage (vs, ps; "other" is tried as a
compute shader), the constant segment c[1] from <key>.c1. The Maxwell → SPIR-V translator (tools/dmsh_xlate, built with `dotnet build -c Release -o work/dmsh_xlate`) runs over all of them
and tools/shader_pack.py packs its output. A shader the translator rejects is listed and left out; the host then skips the draws that use it, as it does for a missing library.
Run the title again with --shaders OUT.nxsl; shaders the library lacks are captured by the next run, so coverage grows as more of the game is played."""
import argparse, hashlib, json, shutil, struct, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump'); ap.add_argument('out'); ap.add_argument('--alt-only', action='store_true', help='register each shader only under the hash of its code (the host aliases the program key on the first draw): a library of every shader in the game stays a third of the size')
    ap.add_argument('--work'); ap.add_argument('--xlate', default=str(REPO / 'work' / 'dmsh_xlate' / 'dmsh_xlate.dll'))
    a = ap.parse_args(argv)
    dump = Path(a.dump); work = Path(a.work) if a.work else dump.parent / (dump.name + '_xlate')
    if not Path(a.xlate).is_file(): sys.exit(f'[RED] the translator is not built: {a.xlate} (cd tools/dmsh_xlate && dotnet build -c Release -o ../../work/dmsh_xlate)')
    work.mkdir(parents=True, exist_ok=True)
    jobs, other = [], 0
    for code in sorted(dump.glob('*.code')):
        key = code.stem
        stage = (code.with_suffix('.stage').read_text().strip() if code.with_suffix('.stage').is_file() else 'other')
        if stage == 'other': stage = 'cs'; other += 1
        path = code
        if stage != 'cs':            # a graphics code area starts with a 0x30-byte header (the 0x12345678 magic); the 0x50-byte SPH after it is read by the translator itself, as nxnative's dmsh_build does
            path = work / (key + '.gfx'); path.write_bytes(code.read_bytes()[0x30:])
        job = {'key': key, 'stage': stage, 'code': str(path), 'debug': False, 'orig': str(code)}
        c1 = code.with_suffix('.c1'); ctl = code.with_suffix('.ctl')
        data = code.read_bytes()
        if ctl.is_file() and len(ctl.read_bytes()) >= 0x708:        # c[1] is the end of the code area, from control +0x700 (older dumps took the 0x100 bytes before the control section, which are not the constants)
            c1o, c1t = struct.unpack_from('<2I', ctl.read_bytes(), 0x700)
            if c1o <= c1t <= len(data): job['c1'] = data[c1o:c1t].hex()
        if 'c1' not in job and c1.is_file(): job['c1'] = c1.read_bytes().hex()
        if stage == 'cs': job['local'] = [64, 1, 1]
        jobs.append(job)
    if not jobs: sys.exit('[RED] no captured shaders in ' + str(dump))
    (work / 'jobs.json').write_text(json.dumps(jobs))
    print(f'{len(jobs)} shaders ({other} without a graphics program header, translated as compute)')
    p = subprocess.run(['dotnet', a.xlate, str(work / 'jobs.json'), str(work / 'out')], capture_output=True, text=True)
    print(p.stdout.strip().splitlines()[-1] if p.stdout.strip() else p.stderr.strip()[-300:])
    for job in jobs:                 # the constant segment c[1] goes into the library beside each shader (shader_pack.py reads <key>.c1)
        if 'c1' in job and (work / 'out').is_dir(): (work / 'out' / (job['key'] + '.c1')).write_bytes(bytes.fromhex(job['c1']))
    out = work / 'out'; nalt = 0
    for job in jobs:                 # every shader also under the key of its code alone (host: prog_alt_key): the normal key hashes control bytes that differ between runs
        k = job['key']; src = out / (k + '.spv')
        if not src.is_file() or not (out / (k + '.res.json')).is_file(): continue
        ak = hashlib.sha256(Path(job['orig']).read_bytes()).digest()[:8].hex()
        if ak == k or (out / (ak + '.spv')).is_file(): continue
        for ext in ('.spv', '.res.json', '.c1'):
            if (out / (k + ext)).is_file(): (shutil.move if a.alt_only else shutil.copyfile)(out / (k + ext), out / (ak + ext))
        nalt += 1
    print(f'{nalt} code-keyed copies')
    r = subprocess.run([sys.executable, str(REPO / 'tools' / 'shader_pack.py'), a.out, str(work / 'out')], capture_output=True, text=True)
    print((r.stdout or r.stderr).strip()[-400:])
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
