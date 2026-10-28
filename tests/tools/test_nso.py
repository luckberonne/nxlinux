"""nso.load() on synthetic modules built here: no game data. Covers a plain and an LZ4 NSO, hash checking, and rejection of damaged files."""
import hashlib, os, struct, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools'))
import lz4.block
import nso

def make_nso(segs, bss=0x2000, compress=False, check_hash=True, build_id=bytes(range(32))):
    """segs: three byte strings (text, ro, data), placed on consecutive pages in memory"""
    flags = 0
    stored, hdr = [], bytearray(0x100)
    hdr[0:4] = b'NSO0'
    off, mem = 0x100, 0
    for i, s in enumerate(segs):
        raw = lz4.block.compress(s, store_size=False) if compress else s
        if compress: flags |= 1 << i
        if check_hash: flags |= 1 << (3 + i)
        struct.pack_into('<III', hdr, 0x10 + i * 0x10, off, mem, len(s))
        struct.pack_into('<I', hdr, 0x60 + i * 4, len(raw))
        hdr[0xa0 + i * 0x20:0xa0 + i * 0x20 + 0x20] = hashlib.sha256(s).digest()
        stored.append(raw); off += len(raw); mem += (len(s) + 0xfff) & ~0xfff
    struct.pack_into('<I', hdr, 0xc, flags)
    struct.pack_into('<I', hdr, 0x3c, bss)
    hdr[0x40:0x60] = build_id
    return bytes(hdr) + b''.join(stored)

SEGS = (b'\x1f\x20\x03\xd5' * 0x400, b'rodata' * 0x100, b'data' * 0x80)

class T(unittest.TestCase):
    def check(self, m):
        self.assertEqual(m.bss, 0x2000)
        self.assertEqual(m.build_id, bytes(range(32)))
        self.assertEqual(m.segments[0], (0, len(SEGS[0])))
        self.assertEqual(bytes(m.image[m.segments[1][0]:m.segments[1][0] + len(SEGS[1])]), SEGS[1])
        self.assertEqual(bytes(m.image[m.segments[2][0]:m.segments[2][0] + len(SEGS[2])]), SEGS[2])
    def test_plain(self): self.check(nso.load(make_nso(SEGS)))
    def test_lz4(self): self.check(nso.load(make_nso(SEGS, compress=True)))
    def test_bad_hash(self):
        d = bytearray(make_nso(SEGS)); d[0x100 + 5] ^= 1
        with self.assertRaises(nso.Error): nso.load(bytes(d))
    def test_truncated(self):
        with self.assertRaises(nso.Error): nso.load(make_nso(SEGS)[:0x200])
    def test_unhashed_ok(self):
        d = bytearray(make_nso(SEGS, check_hash=False)); d[0x100 + 5] ^= 1
        nso.load(bytes(d))

if __name__ == '__main__': unittest.main()
