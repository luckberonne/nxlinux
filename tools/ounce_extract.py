#!/usr/bin/env python3
"""Extract the shader programs of an Ounce (Switch 2, NVN2) title's shader archives.

The archives are Shader/*.bfsha.zs in the RomFS: zstd with the dictionary zs.zsdic of Pack/ZsDic.pack.zs, then FSHA > BNSH, whose stage binaries are an NVN control section
(magic 0x98761234, 0xe0 bytes or a little more, holding the shader's own 16-byte hash at +0x20 and the sizes) and the code area: a 0x80-byte program header and plain SM86 SASS
(16-byte instructions). Each control section is referenced by a record {u64 control offset, u64 code offset, ...} relative to the BNSH.

  ounce_extract.py ROMFS.img OUT [--only SUBSTR]      writes OUT/<hash16>.code (header and instructions), .ctl (the control section), .c1 (the constant segment: jump tables) and prints a census
  ounce_extract.py FILE.bfsha OUT                      an archive already decompressed

No game data is kept in the repository: the output goes where the caller says."""
import argparse, collections, os, struct, sys
from pathlib import Path

CTL_MAGIC = 0x98761234


def romfs_files(img):
    """Yield (path, offset, size) of every file of a RomFS image."""
    f = open(img, 'rb')
    h = struct.unpack('<QQQQQQQQQQ', f.read(0x50))
    dmo, dms, fmo, fms, dat = h[3], h[4], h[7], h[8], h[9]
    f.seek(dmo); dirs = f.read(dms); f.seek(fmo); files = f.read(fms)

    def dpath(o):
        p = []
        while o != 0xFFFFFFFF:
            par, _s, _c, _f, _h, nl = struct.unpack_from('<IIIIII', dirs, o)
            n = dirs[o + 24:o + 24 + nl].decode()
            if n: p.append(n)
            o = par if par != o else 0xFFFFFFFF
        return '/'.join(reversed(p))
    o = 0
    while o < len(files):
        par, _s, off, size, _h, nl = struct.unpack_from('<IIQQII', files, o)
        yield dpath(par) + '/' + files[o + 32:o + 32 + nl].decode(), dat + off, size
        o += 32 + ((nl + 3) & ~3)
    f.close()


def zstd_tools():
    try:
        from compression.zstd import decompress, ZstdDict
    except ImportError:
        sys.exit('needs Python 3.14 (compression.zstd)')
    return decompress, ZstdDict


def load_dictionary(img, ZstdDict, decompress):
    """zs.zsdic from Pack/ZsDic.pack.zs (a SARC of dictionaries, itself zstd without a dictionary)."""
    with open(img, 'rb') as f:
        for path, off, size in romfs_files(img):
            if path.endswith('Pack/ZsDic.pack.zs'):
                f.seek(off); sarc = decompress(f.read(size)); break
        else:
            sys.exit('Pack/ZsDic.pack.zs not found in the RomFS')
    e = '<' if struct.unpack_from('>H', sarc, 6)[0] == 0xFFFE else '>'
    data_off, = struct.unpack_from(e + 'I', sarc, 0xC)
    cnt, = struct.unpack_from(e + 'H', sarc, 0x1A)
    names = 0x14 + 0xC + 16 * cnt + 8
    for i in range(cnt):
        _h, no, s, en = struct.unpack_from(e + 'IIII', sarc, 0x20 + 16 * i)
        no = (no & 0xFFFFFF) * 4
        if sarc[names + no:sarc.index(b'\0', names + no)] == b'zs.zsdic':
            return ZstdDict(sarc[data_off + s:data_off + en])
    sys.exit('zs.zsdic not in ZsDic.pack')


def programs(d):
    """Yield dicts for every stage binary of a decompressed FSHA: hash, control, code (header + instructions)."""
    if d[:4] != b'FSHA': raise ValueError('not an FSHA archive')
    b = d.find(b'BNSH')
    if b < 0: return
    magic = struct.pack('<I', CTL_MAGIC)
    ctrls = []
    i = d.find(magic, b)
    while i >= 0:
        if i % 4 == 0: ctrls.append(i)
        i = d.find(magic, i + 1)
    want = {c - b: c for c in ctrls}
    q = struct.unpack_from('<%dQ' % ((len(d) - b) // 8), d, b)
    code_off = {}
    for k, v in enumerate(q):
        if v in want and k + 1 < len(q) and v not in code_off:
            code_off[v] = q[k + 1]
    for rel, c in want.items():
        co = code_off.get(rel)
        size, = struct.unpack_from('<I', d, c + 0x38)
        ctl_size, = struct.unpack_from('<I', d, c + 0x30)
        if co is None or not 0x80 < size < (1 << 24) or b + co + size > len(d):
            yield {'ctl_at': c, 'error': 'no code record' if co is None else 'bad size %#x' % size}
            continue
        code = d[b + co:b + co + size]
        # The area after the code is the shader's constant segment c[1] (jump tables of BRX): control +0x40 is where it starts, +0x44 the end of the area
        c1_at, area = struct.unpack_from('<II', d, c + 0x40)
        c1 = d[b + co + c1_at:b + co + area] if size <= c1_at <= area and b + co + area <= len(d) else b''
        yield {'ctl_at': c, 'hash': d[c + 0x20:c + 0x30], 'control': d[c:c + max(ctl_size, 0xe0)], 'code': code, 'c1': c1, 'word0': struct.unpack_from('<I', code, 0)[0]}


def adjacent_programs(d):
    """Stage binaries of the other containers (the NDVE movie engine's .dat, the release.sarc and effect .vsetb archives): the code area follows its control section at once (control +0x30 is the
    section's size, +0x38 the code size). A control with a hash is keyed by it; one without (the programs a title builds itself, the effects') is keyed by the SHA-256 of its code, as the host does."""
    import hashlib
    magic = struct.pack('<I', CTL_MAGIC)
    i = d.find(magic)
    while i >= 0:
        if i % 4 == 0:
            ctl_size, = struct.unpack_from('<I', d, i + 0x30); size, = struct.unpack_from('<I', d, i + 0x38)
            if 0xe0 <= ctl_size <= 0x8000 and 0x80 < size < (1 << 24) and i + ctl_size + size <= len(d):
                code = d[i + ctl_size:i + ctl_size + size]
                c1_at, area = struct.unpack_from('<II', d, i + 0x40)
                c1 = code[c1_at:area] if size <= c1_at <= area <= len(code) else b''
                h = d[i + 0x20:i + 0x28]
                yield {'key': (h if h != bytes(8) else hashlib.sha256(code).digest()[:8]).hex(), 'control': d[i:i + ctl_size], 'code': code, 'c1': c1, 'word0': struct.unpack_from('<I', code, 0)[0]}
        i = d.find(magic, i + 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('src'); ap.add_argument('out'); ap.add_argument('--only', default=''); ap.add_argument('--others', action='store_true', help='also the other containers of the RomFS (adjacent_programs)')
    a = ap.parse_args()
    decompress, ZstdDict = zstd_tools()
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    archives = []
    if a.src.endswith('.bfsha'):
        archives.append((a.src, open(a.src, 'rb').read()))
    else:
        zd = load_dictionary(a.src, ZstdDict, decompress)
        with open(a.src, 'rb') as f:
            for path, off, size in romfs_files(a.src):
                if path.endswith('.bfsha.zs') and a.only in path:
                    f.seek(off); archives.append((path, decompress(f.read(size), zstd_dict=zd)))
    seen = {}; stages = collections.Counter(); errors = 0
    for path, d in archives:
        n = 0
        for p in programs(d):
            if 'error' in p: errors += 1; continue
            key = p['hash'][:8].hex()
            if key not in seen:
                seen[key] = path
                (out / (key + '.code')).write_bytes(p['code']); (out / (key + '.ctl')).write_bytes(p['control']); (out / (key + '.c1')).write_bytes(p['c1'])
            stages[p['word0'] & 0x1f] += 1; n += 1
        print(f'{path}: {len(d):#x} bytes, {n} stage binaries')
    if a.others and not a.src.endswith('.bfsha'):
        magic = struct.pack('<I', CTL_MAGIC); n = 0
        with open(a.src, 'rb') as f:
            for path, off, size in romfs_files(a.src):
                if path.endswith('.bfsha.zs') or size > 64 << 20 or path.startswith(('Event/Movie', 'PreComputedCulling', 'Sound/')) or a.only not in path: continue
                f.seek(off); raw = f.read(size)
                try: d = decompress(raw, zstd_dict=zd) if path.endswith('.zs') else raw
                except Exception: d = raw
                if magic not in d: continue
                for p in adjacent_programs(d):
                    if p['key'] not in seen:
                        seen[p['key']] = path; n += 1
                        (out / (p['key'] + '.code')).write_bytes(p['code']); (out / (p['key'] + '.ctl')).write_bytes(p['control']); (out / (p['key'] + '.c1')).write_bytes(p['c1'])
                        stages[p['word0'] & 0x1f] += 1
        print(f'{n} more programs from the other containers')
    print(f'{len(seen)} distinct programs written to {out}; {errors} control sections without a usable record; SPH word0 low 5 bits: {dict(stages)}')


if __name__ == '__main__':
    main()
