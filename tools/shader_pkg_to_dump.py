#!/usr/bin/env python3
"""Turns the package directory nxnative's shader_split.py writes (every shader the game files hold) into the dump directory tools/shader_capture_build.py reads.

  shader_pkg_to_dump.py PKG_DIR OUT_DUMP_DIR

A package file <stage>_<group>.bin is a run of records: 'BTCDBTCD', a 0x30-byte header (control length at +0x28, code length at +0x2c), the code area, the control section.
For each vertex, pixel and compute record this writes <key>.code, <key>.ctl and <key>.stage, <key> being the 8-byte key the host computes (hle_nvn.c keys_hash: SHA-256 of the control
section's first min(0x880, its own length at +0x20) bytes, then the code area). The key is only a file name here: the builder also registers every shader under the hash of its code alone."""
import hashlib, re, struct, sys
from pathlib import Path

STAGE = {'vs': 'vs', 'ps': 'ps', 'cs': 'other'}       # the builder translates 'other' as compute; hull, domain and geometry stages are left out


def records(d):
    at = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(at[k + 1] if k + 1 < len(at) else len(d))] for k, x in enumerate(at)]


def host_key(ctl, code):
    nctl = 0x880
    own = struct.unpack_from('<I', ctl, 0x20)[0] if len(ctl) >= 0x24 else 0
    if 0x760 <= own < 0x880: nctl = own
    h = hashlib.sha256(); h.update(ctl[:nctl].ljust(nctl, b'\0')); h.update(code)
    return h.digest()[:8].hex()


def main(argv):
    if len(argv) != 3: sys.exit(__doc__)
    pkg, out = Path(argv[1]), Path(argv[2]); out.mkdir(parents=True, exist_ok=True)
    n = skipped = 0
    for f in sorted(pkg.glob('*.bin')):
        stage = f.name.split('_', 1)[0]
        if stage not in STAGE: skipped += len(records(f.read_bytes())); continue
        for rec in records(f.read_bytes()):
            clen, codelen = struct.unpack_from('<2I', rec, 0x28)
            code = rec[0x38:0x38 + codelen]; ctl = rec[0x38 + codelen:0x38 + codelen + clen]
            if len(code) != codelen or len(ctl) != clen: sys.exit(f'[RED] {f.name}: a record is shorter than its header says')
            key = host_key(ctl, code)
            (out / (key + '.code')).write_bytes(code)
            (out / (key + '.ctl')).write_bytes(ctl[:0x880])
            (out / (key + '.stage')).write_text(STAGE[stage])
            n += 1
    print(f'{n} shaders written to {out} ({skipped} of other stages left out)')


if __name__ == '__main__':
    main(sys.argv)
