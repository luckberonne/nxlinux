#!/usr/bin/env python3
"""Builds the shader library tests/test_nvn_draw.cpp loads for its last two stages: a Maxwell fragment shader that writes constant orange, and a vertex/fragment pair that
passes a position and a colour through, assembled by tools/maxwell_asm.py,
translated to SPIR-V by tools/dmsh_xlate and packed by tools/shader_pack.py.

Usage: make_maxwell_lib.py dmsh_xlate.dll out.nxsl workdir

The shader's key is the one hle_nvn.c computes for the stand-in the guest program hands the NVN API: a control section of 0x880 bytes of 0x44 (own length at +0x20, code
size 64 at +0x704) and 64 bytes of 0xE5 as its code. The bytes the translator reads are the assembled ones; the key is only the name they are filed under."""
import hashlib, json, struct, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import maxwell_asm, shader_pack


def stand_in_key(fill_control=0x44, fill_code=0xE5, ncode=64):
    ctl = bytearray([fill_control]) * 0x880
    struct.pack_into('<I', ctl, 0x20, 0x880); struct.pack_into('<I', ctl, 0x704, ncode)
    return hashlib.sha256(bytes(ctl) + bytes([fill_code]) * ncode).digest()[:8].hex()


def translate(dll, jobs, workdir):
    """jobs: [{'key', 'stage', 'code': bytes, ...}] -> directory of <key>.spv/<key>.res.json"""
    workdir = Path(workdir); out = workdir / 'out'; workdir.mkdir(parents=True, exist_ok=True)
    spec = []
    for j in jobs:
        code = workdir / f"{j['key']}.bin"; code.write_bytes(j['code'])
        spec.append({**{k: v for k, v in j.items() if k != 'code'}, 'code': str(code)})
    (workdir / 'job.json').write_text(json.dumps(spec))
    subprocess.run(['dotnet', str(dll), str(workdir / 'job.json'), str(out)], check=True, stdout=subprocess.DEVNULL)
    return out


def main(argv):
    if len(argv) != 4: sys.exit(__doc__)
    dll, lib, work = argv[1:]
    out = translate(dll, [
        {'key': stand_in_key(), 'stage': 'ps', 'code': maxwell_asm.fragment_constant(1.0, 0.5, 0.0, 1.0), 'fout': {'0': 'float'}},
        {'key': stand_in_key(0x55, 0xF6), 'stage': 'vs', 'code': maxwell_asm.vertex_passthrough(), 'attr': {'0': 'float', '1': 'float'}},
        {'key': stand_in_key(0x66, 0x07), 'stage': 'ps', 'code': maxwell_asm.fragment_varying(), 'fout': {'0': 'float'}},
        {'key': stand_in_key(0x77, 0x18), 'stage': 'vs', 'code': maxwell_asm.vertex_shifted(), 'attr': {'0': 'float', '1': 'float'}},
        {'key': stand_in_key(0x88, 0x29), 'stage': 'ps', 'code': maxwell_asm.fragment_textured(0), 'fout': {'0': 'float'}, 'tex': {'0': '2d'}}], work)
    shader_pack.main(['shader_pack.py', lib, str(out)])


if __name__ == '__main__':
    main(sys.argv)
