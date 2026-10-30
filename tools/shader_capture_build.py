#!/usr/bin/env python3
"""Turns the shaders a run captured (NX_SHADER_DUMP=<dir>, see hle_nvn.c shader_dump) into a shader library the host loads with --shaders.

  shader_capture_build.py DUMP_DIR OUT.nxsl [--work DIR] [--xlate DLL]

For every <key>.code in DUMP_DIR (the key is the one the host computes from the control section and the code): the stage comes from <key>.stage (vs, ps; "other" is tried as a
compute shader), the constant segment c[1] from <key>.c1. The Maxwell → SPIR-V translator (tools/dmsh_xlate, built with `dotnet build -c Release -o work/dmsh_xlate`) runs over all of them
and tools/shader_pack.py packs its output. A shader the translator rejects is listed and left out; the host then skips the draws that use it, as it does for a missing library.
Run the title again with --shaders OUT.nxsl; shaders the library lacks are captured by the next run, so coverage grows as more of the game is played."""
import argparse, json, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump'); ap.add_argument('out')
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
        job = {'key': key, 'stage': stage, 'code': str(path), 'debug': False}
        c1 = code.with_suffix('.c1')
        if c1.is_file(): job['c1'] = c1.read_bytes().hex()
        if stage == 'cs': job['local'] = [64, 1, 1]
        jobs.append(job)
    if not jobs: sys.exit('[RED] no captured shaders in ' + str(dump))
    (work / 'jobs.json').write_text(json.dumps(jobs))
    print(f'{len(jobs)} shaders ({other} without a graphics program header, translated as compute)')
    p = subprocess.run(['dotnet', a.xlate, str(work / 'jobs.json'), str(work / 'out')], capture_output=True, text=True)
    print(p.stdout.strip().splitlines()[-1] if p.stdout.strip() else p.stderr.strip()[-300:])
    for job in jobs:                 # the constant segment c[1] goes into the library beside each shader (shader_pack.py reads <key>.c1)
        if 'c1' in job and (work / 'out').is_dir(): (work / 'out' / (job['key'] + '.c1')).write_bytes(bytes.fromhex(job['c1']))
    r = subprocess.run([sys.executable, str(REPO / 'tools' / 'shader_pack.py'), a.out, str(work / 'out')], capture_output=True, text=True)
    print((r.stdout or r.stderr).strip()[-400:])
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
