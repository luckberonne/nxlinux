#!/usr/bin/env python3
"""Packs the translator's output (<key>.spv + <key>.res.json, see tools/dmsh_xlate) into one shader library the host loads with nvn_vk_shader_load_library.

Usage: shader_pack.py out.nxsl <translator output dir>          (every <key>.spv with a <key>.res.json beside it; <key> is the 16 hex digits of the 8-byte shader key)

Library (little endian): "NXSL", u32 version (2), u32 count; per shader: key[8], u32 stage (NVN_STAGE_*), u32 needs, u32 nbinds, u32 spirv bytes, then nbinds x
{u32 binding, u32 kind, u32 reg, u32 aux}, then the SPIR-V. `needs` marks what the shader reads that the draw path cannot supply yet, so the host can say so and skip the draw
instead of running it wrong:
  2 the shader's own constant segment c[1]   8 storage buffers   16 textures   32 images   64 a geometry shader's helper buffers
  (1 and 4, the driver segments c[0] and c[2], are bindings now: kinds 4 and 5, filled by the host per draw; so is the translator's support buffer, kind 3, when the module reads it)
Uniform buffers c[3 + reg] are plain bindings (kind 1 = cbuf, reg = the NVN uniform slot); a plain 2D texture read through a combined handle is kind 2 (reg = its c[2] slot,
aux = the SamplerType).
"""
import json, struct, sys
from pathlib import Path

STAGE = {'vs': 0, 'ps': 1, 'cs': 5}          # NVN_STAGE_VERTEX, FRAGMENT, COMPUTE
KIND_CBUF, KIND_TEX, KIND_SUPPORT, KIND_DRV0, KIND_DRV2, KIND_SBUF = 1, 2, 3, 4, 5, 6
C0_SSBO = {'vs': 0x110, 'cs': 0x310, 'gs': 0x410, 'ps': 0x510}      # the stage's storage buffer table in driver segment c[0] (src/gfx/nvn/dmsh_abi.h): 16 slots of 0x10 bytes
C0_SSBO_SLOTS = 16
SAMPLER_TYPES = {1, 3, 4, 5}                 # the translator's SamplerType 1D, 2D, 3D and cube; Array (256) and Shadow (1024) may be added, Multisample (512) and buffers (2) may not
C2_TEX, C2_SEPTEX = 0x20, 0x168               # byte offsets in driver segment c[2] (src/gfx/nvn/dmsh_abi.h): combined handles, and where the separate textures start


def combined_slot(t):
    """The c[2] combined-texture slot of a 1D, 2D, 3D or cube texture (or array of them) read through a combined handle, or None for anything the draw path cannot bind yet.
    The reflection's `handle` packs the cbuf word offsets: bits 0-13 the first, 14-27 the second, 28-31 the handle type (0 = combined sampler); cbuf_slot -1 is a texture bound the usual way (its handle is in c[2])."""
    h = t['handle']
    if t.get('separate') or t.get('array', 1) > 1 or t['cbuf_slot'] not in (-1, 2) or (h >> 28) != 0 or (t['type'] & 0xFF) not in SAMPLER_TYPES or t['type'] & 512 or (t['type'] & 0xFF) == 4 and t['type'] & 256: return None
    b = (h & 0x3FFF) * 4
    if b % 8 or not C2_TEX <= b < C2_SEPTEX: return None
    return (b - C2_TEX) // 8
NEEDS_C0, NEEDS_C1, NEEDS_C2, NEEDS_SBUF, NEEDS_TEX, NEEDS_IMG, NEEDS_GEOM = 1, 2, 4, 8, 16, 32, 64


def uniform_blocks(spirv):
    """{binding: constant buffer slot} of the translator's uniform blocks, named <stage>_c<slot>. The reflection lists a block only when an instruction read it directly; the driver
    segment c[0] a storage buffer's address comes from has a binding all the same, which only the module itself shows."""
    w = struct.unpack(f'<{len(spirv) // 4}I', spirv)
    names, bindings = {}, {}
    i = 5
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        if n == 0: break
        if op == 5: names[w[i + 1]] = struct.pack(f'<{n - 2}I', *w[i + 2:i + n]).split(b'\0')[0].decode(errors='replace')
        elif op == 71 and w[i + 2] == 33: bindings[w[i + 1]] = w[i + 3]
        i += n
    out = {}
    for k, nm in names.items():
        parts = nm.split('_c')
        if len(parts) == 2 and parts[0] in ('vp', 'fp', 'cp', 'gp', 'tcp', 'tep') and parts[1].isdigit() and k in bindings: out[bindings[k]] = int(parts[1])
    return out


def support_binding(spirv):
    """The binding of the translator's support buffer when the module reads it (render scale, viewport size, alpha test...), else None. It is the uniform block named
    support_buffer; a module declares it always, so only a load through it counts."""
    w = struct.unpack(f'<{len(spirv) // 4}I', spirv)
    names, bindings, ptr_id = {}, {}, None
    i = 5
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        if n == 0: break
        if op == 5:                                                    # OpName
            nm = struct.pack(f'<{n - 2}I', *w[i + 2:i + n]).split(b'\0')[0]
            names[w[i + 1]] = nm.decode(errors='replace')
        elif op == 71 and w[i + 2] == 33: bindings[w[i + 1]] = w[i + 3]            # OpDecorate Binding
        i += n
    ptr_id = next((k for k, v in names.items() if v == 'support_buffer'), None)
    if ptr_id is None: return None
    i = 5
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        if n == 0: break
        if op in (65, 66, 61) and w[i + 3] == ptr_id:                  # OpAccessChain / OpInBoundsAccessChain / OpLoad of it
            return bindings.get(ptr_id, 0)
        i += n
    return None


def entry(key_hex, spirv, res):
    """One library entry from a translated shader"""
    key = bytes.fromhex(key_hex)
    if len(key) != 8: raise ValueError(f'a shader key is 8 bytes, got {key_hex!r}')
    needs, binds = 0, []
    for binding, slot in sorted(uniform_blocks(spirv).items()):
        if slot == 0: binds.append((binding, KIND_DRV0, 0, 0))
        elif slot == 1: needs |= NEEDS_C1
        elif slot == 2: binds.append((binding, KIND_DRV2, 0, 0))
        else: binds.append((binding, KIND_CBUF, slot - 3, 0))
    for cb in res.get('cbufs', []):
        if cb['binding'] not in (b[0] for b in binds):                 # a block the module does not name the usual way
            slot = cb['slot']
            if slot == 0: binds.append((cb['binding'], KIND_DRV0, 0, 0))
            elif slot == 1: needs |= NEEDS_C1
            elif slot == 2: binds.append((cb['binding'], KIND_DRV2, 0, 0))
            else: binds.append((cb['binding'], KIND_CBUF, slot - 3, 0))
    for sb in res.get('sbufs', []):
        base = C0_SSBO.get(res['stage'])
        if base is not None and sb['cb_slot'] == 0 and sb.get('cb_offset', 0) * 4 == base + 0x10 * sb['slot'] and sb['slot'] < C0_SSBO_SLOTS:
            binds.append((sb['binding'], KIND_SBUF, sb['slot'], sb.get('flags', 0)))
        else: needs |= NEEDS_SBUF                                         # an address kept elsewhere (the game's own uniform buffer)
    for t in res.get('textures', []):
        slot = combined_slot(t)
        if slot is None: needs |= NEEDS_TEX
        else: binds.append((t['binding'], KIND_TEX, slot, t['type']))
    if res.get('images'): needs |= NEEDS_IMG
    if res.get('geometry_stride'): needs |= NEEDS_GEOM
    if len(spirv) % 4: raise ValueError('SPIR-V is a whole number of words')
    sb = support_binding(spirv)
    if sb is not None: binds.append((sb, KIND_SUPPORT, 0, 0))
    stage = STAGE[res['stage']]
    return key + struct.pack('<4I', stage, needs, len(binds), len(spirv)) + b''.join(struct.pack('<4I', *b) for b in binds) + spirv


def pack(entries):
    return b'NXSL' + struct.pack('<II', 2, len(entries)) + b''.join(entries)


def unpack(data):
    """The inverse, for tests: [{key, stage, needs, binds, spirv}]"""
    if data[:4] != b'NXSL': raise ValueError('not a shader library')
    ver, n = struct.unpack_from('<II', data, 4)
    if ver != 2: raise ValueError(f'library version {ver}')
    off, out = 12, []
    for _ in range(n):
        key = data[off:off + 8]; stage, needs, nb, nbytes = struct.unpack_from('<4I', data, off + 8); off += 24
        binds = [struct.unpack_from('<4I', data, off + 16 * i) for i in range(nb)]; off += 16 * nb
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
