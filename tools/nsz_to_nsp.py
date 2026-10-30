#!/usr/bin/env python3
"""Turns an NSZ (an NSP whose big NCAs are NCZ: zstd-compressed and decrypted) back into a plain NSP the extractor reads.

  nsz_to_nsp.py in.nsz out.nsp

Each .ncz is rebuilt into the original NCA: the first 0x4000 bytes (the encrypted header) are stored as is, the rest is the decompressed stream, and every section the NCZ
lists with AES-CTR crypto (types 3 and 4) is encrypted again with the key and counter the NCZ keeps. Needs the `zstandard` and `cryptography` packages (the project's .venv has them).
Memory stays small: the stream is handled in 8 MiB pieces."""
import struct, sys
import zstandard
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HEADER = 0x4000
PIECE = 8 << 20


def read_pfs0(f):
    magic, n, strsz, _ = struct.unpack('<4sIII', f.read(16))
    if magic != b'PFS0': raise SystemExit('not a PFS0 container')
    ents = [struct.unpack('<QQII', f.read(24)) for _ in range(n)]
    names = f.read(strsz)
    base = 16 + 24 * n + strsz
    out = []
    for off, size, noff, _ in ents:
        out.append((names[noff:names.index(b'\0', noff)].decode(), base + off, size))
    return out


def write_pfs0(path, items):
    """items: [(name, size, writer(out_file))]."""
    names = b''.join(n.encode() + b'\0' for n, _, _ in items)
    names += b'\0' * (-(16 + 24 * len(items) + len(names)) % 0x20)
    hdr = struct.pack('<4sIII', b'PFS0', len(items), len(names), 0)
    off = 0; table = b''; noff = 0
    for n, size, _ in items:
        table += struct.pack('<QQII', off, size, noff, 0); off += size; noff += len(n.encode()) + 1
    with open(path, 'wb') as o:
        o.write(hdr + table + names)
        for _, _, w in items: w(o)


class Encryptor:
    def __init__(self, sections): self.sections = sections

    def crypt(self, pos, data):
        """encrypts data that sits at absolute NCA offset pos"""
        out = bytearray(); i = 0
        while i < len(data):
            sec = next((s for s in self.sections if s[0] <= pos + i < s[0] + s[1]), None)
            end = min(len(data), (sec[0] + sec[1] - pos) if sec else len(data))
            chunk = data[i:end]
            if sec and sec[2] in (3, 4):
                a = pos + i
                ctr = bytearray(sec[4]); ctr[8:16] = (a >> 4).to_bytes(8, 'big')
                enc = Cipher(algorithms.AES(sec[3]), modes.CTR(bytes(ctr[:16]) if a % 16 == 0 else bytes(ctr))).encryptor()
                if a % 16: enc.update(b'\0' * (a % 16))
                chunk = enc.update(bytes(chunk))[(a % 16):] if a % 16 else enc.update(bytes(chunk))
            out += chunk; i = end
        return bytes(out)


def ncz_to_nca(src, start, size, out):
    src.seek(start)
    out.write(src.read(HEADER))
    if src.read(8) != b'NCZSECTN': raise SystemExit('no NCZSECTN: not an NCZ')
    count, = struct.unpack('<Q', src.read(8))
    sections = []
    for _ in range(count):
        off, sz, ct, _pad = struct.unpack('<QQQQ', src.read(32)); key = src.read(16); ctr = src.read(16)
        sections.append((off, sz, ct, key, ctr))
    enc = Encryptor(sections)
    mark = src.tell(); tag = src.read(8)
    pos = HEADER
    if tag == b'NCZBLOCK':
        _ver, _typ, _unused, bexp = struct.unpack('<BBBB', src.read(4))
        nblocks, total = struct.unpack('<IQ', src.read(12))
        sizes = struct.unpack('<%dI' % nblocks, src.read(4 * nblocks)); bsz = 1 << bexp
        dctx = zstandard.ZstdDecompressor()
        for i, cs in enumerate(sizes):
            blk = src.read(cs); want = min(bsz, total - i * bsz)
            data = blk if cs == want else dctx.decompress(blk, max_output_size=want)
            out.write(enc.crypt(pos, data)); pos += len(data)
    else:
        src.seek(mark)
        rd = zstandard.ZstdDecompressor().stream_reader(src, read_across_frames=True)
        while True:
            data = rd.read(PIECE)
            if not data: break
            out.write(enc.crypt(pos, data)); pos += len(data)
    return pos


def main():
    if len(sys.argv) != 3: raise SystemExit(__doc__)
    src = open(sys.argv[1], 'rb')
    ents = read_pfs0(src)
    items = []
    for name, start, size in ents:
        if name.endswith('.ncz'):
            # the rebuilt size is only known after decompressing: do it once into a temporary file next to the output
            tmp = sys.argv[2] + '.' + name + '.tmp'
            with open(tmp, 'wb') as t: total = ncz_to_nca(src, start, size, t)
            def w(o, tmp=tmp):
                with open(tmp, 'rb') as t:
                    while True:
                        b = t.read(PIECE)
                        if not b: break
                        o.write(b)
            items.append((name[:-4] + '.nca', total, w))
        else:
            def w(o, start=start, size=size):
                src.seek(start); left = size
                while left:
                    b = src.read(min(left, PIECE)); o.write(b); left -= len(b)
            items.append((name, size, w))
    write_pfs0(sys.argv[2], items)
    import os
    for name, _, _ in ents:
        t = sys.argv[2] + '.' + name + '.tmp'
        if os.path.exists(t): os.remove(t)
    print('wrote', sys.argv[2])


if __name__ == '__main__':
    main()
