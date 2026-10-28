#!/usr/bin/env python3
"""Packs the translator's output (<key>.spv + <key>.res.json, see tools/dmsh_xlate) into one shader library the host loads with nvn_vk_shader_load_library.

Usage: shader_pack.py out.nxsl <translator output dir>          (every <key>.spv with a <key>.res.json beside it; <key> is the 16 hex digits of the 8-byte shader key)

Library (little endian): "NXSL", u32 version (1), u32 count; per shader: key[8], u32 stage (NVN_STAGE_*), u32 needs, u32 nbinds, u32 spirv bytes, then nbinds x
{u32 binding, u32 kind, u32 reg}, then the SPIR-V. `needs` marks what the shader reads that the draw path cannot supply yet, so the host can say so and skip the draw
instead of running it wrong:
  1 driver segment c[0] (storage buffer table, point size, base instance)   2 the shader's own constant segment c[1]   4 driver segment c[2] (texture and sampler handles)
  8 storage buffers   16 textures   32 images   64 a geometry shader's helper buffers
Uniform buffers c[3 + reg] are plain bindings (kind 1 = cbuf, reg = the NVN uniform slot).
"""
import json, struct, sys
from pathlib import Path

STAGE = {'vs': 0, 'ps': 1, 'cs': 5}          # NVN_STAGE_VERTEX, FRAGMENT, COMPUTE
KIND_CBUF = 1
NEEDS_C0, NEEDS_C1, NEEDS_C2, NEEDS_SBUF, NEEDS_TEX, NEEDS_IMG, NEEDS_GEOM = 1, 2, 4, 8, 16, 32, 64


def entry(key_hex, spirv, res):
    """One library entry from a translated shader"""
    key = bytes.fromhex(key_hex)
    if len(key) != 8: raise ValueError(f'a shader key is 8 bytes, got {key_hex!r}')
    needs, binds = 0, []
    for cb in res.get('cbufs', []):
        slot = cb['slot']
        if slot == 0: needs |= NEEDS_C0
        elif slot == 1: needs |= NEEDS_C1
        elif slot == 2: needs |= NEEDS_C2
        else: binds.append((cb['binding'], KIND_CBUF, slot - 3))
    if res.get('sbufs'): needs |= NEEDS_SBUF
    if res.get('textures'): needs |= NEEDS_TEX
    if res.get('images'): needs |= NEEDS_IMG
    if res.get('geometry_stride'): needs |= NEEDS_GEOM
    if len(spirv) % 4: raise ValueError('SPIR-V is a whole number of words')
    stage = STAGE[res['stage']]
    return key + struct.pack('<4I', stage, needs, len(binds), len(spirv)) + b''.join(struct.pack('<3I', *b) for b in binds) + spirv


def pack(entries):
    return b'NXSL' + struct.pack('<II', 1, len(entries)) + b''.join(entries)


def unpack(data):
    """The inverse, for tests: [{key, stage, needs, binds, spirv}]"""
    if data[:4] != b'NXSL': raise ValueError('not a shader library')
    ver, n = struct.unpack_from('<II', data, 4)
    if ver != 1: raise ValueError(f'library version {ver}')
    off, out = 12, []
    for _ in range(n):
        key = data[off:off + 8]; stage, needs, nb, nbytes = struct.unpack_from('<4I', data, off + 8); off += 24
        binds = [struct.unpack_from('<3I', data, off + 12 * i) for i in range(nb)]; off += 12 * nb
        out.append({'key': key.hex(), 'stage': stage, 'needs': needs, 'binds': binds, 'spirv': data[off:off + nbytes]}); off += nbytes
    return out


def main(argv):
    if len(argv) != 3: sys.exit(__doc__)
    out, d = Path(argv[1]), Path(argv[2])
    entries = []
    for spv in sorted(d.glob('*.spv')):
        if spv.name.endswith('.reader.spv'): continue
        res = spv.with_suffix('.res.json')
        if not res.exists(): sys.exit(f'{spv}: no {res.name} beside it')
        entries.append(entry(spv.stem, spv.read_bytes(), json.loads(res.read_text())))
    out.write_bytes(pack(entries))
    print(f'{out}: {len(entries)} shader(s)')


if __name__ == '__main__':
    main(sys.argv)
