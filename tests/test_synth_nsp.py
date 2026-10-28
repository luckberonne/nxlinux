#!/usr/bin/env python3
"""Tests of tools/synth_nsp.py: the primitives against an OpenSSL-backed reference,
the containers against tools/nso.py and a hand re-parse, and the whole chain
against the vendored nx_extract when it is built at work/nx_extract.

Run with the venv: .venv/bin/python tests/test_synth_nsp.py (needs cryptography)."""
import hashlib
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).parent.parent
sys.path.insert(0, str(REPO / 'tools'))
import synth_nsp as s   # noqa: E402

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes   # noqa: E402


def encrypt(mode, data):
    e = Cipher(algorithms.AES(mode.key), mode.mode).encryptor()
    return e.update(bytes(data)) + e.finalize()


class Xts(unittest.TestCase):
    """My XTS against cryptography's (OpenSSL): data key first, tweak second, one
    stretch = one 512-byte sector (what LibHac's DecryptHeader does per sector)."""

    def test_matches_cryptography(self):
        rng = random.Random(7)
        for sector in (0, 1, 3, 2 ** 32 + 5):
            for _ in range(3):
                key1, key2 = rng.randbytes(16), rng.randbytes(16)
                data = rng.randbytes(512)
                iv = b'\0' * 8 + sector.to_bytes(8, 'big')
                ref = encrypt(_Mode(key1 + key2, modes.XTS(iv)), data)
                self.assertEqual(s.xts_encrypt(data, key1, key2, sector), ref)
                self.assertEqual(s.xts_decrypt(ref, key1, key2, sector), data)


class _Mode:
    def __init__(self, key, mode):
        self.key, self.mode = key, mode


class Ctr(unittest.TestCase):
    """My CTR against cryptography's with the same counters: bytes 0..7 fixed,
    8..15 tick big endian, the first block at first_off/16."""

    def test_matches_cryptography(self):
        rng = random.Random(11)
        key = rng.randbytes(16)
        counter_hi = int.from_bytes(rng.randbytes(8), 'big')
        data = rng.randbytes(1600)
        ref = encrypt(_Mode(key, modes.CTR(counter_hi.to_bytes(8, 'big') + (s.FIRST_SECTION // 16).to_bytes(8, 'big'))), data)
        self.assertEqual(s.ctr_encrypt(data, key, counter_hi, s.FIRST_SECTION), ref)


class Nso(unittest.TestCase):
    """The modules: whatever the builder emits, nso.py must load as Horizon would."""

    def test_loadable_and_layout(self):
        text = b'\x60\x04\x80\x52' + b'\x01\x00\x00\xd4' + b'\xc0\x03\x5f\xd6'
        module = s.build_nso(text, b'(synthetic base module)\0', b'', 0x4000, random.Random(3).randbytes(0x20))
        import nso
        m = nso.load(module, 'synthetic main')
        self.assertEqual(bytes(m.image[:len(text)]), text)
        self.assertEqual(m.bss, 0x4000)

    def test_page_laying(self):
        # uneven sizes: the next memory offset still has to clear the previous end
        text = random.Random(5).randbytes(0x21)
        module = s.build_nso(text, b'ro\0', b'd\0', 0x1000, bytes(0x20))
        import nso
        m = nso.load(module)
        self.assertEqual(bytes(m.image[:0x21]), text)
        self.assertEqual(m.segments[1][0], s._page_up(0x21))


class Pfs0(unittest.TestCase):
    """The container: parse it by hand the way PartitionFileSystemMetaCore does."""

    def parse(self, blob):
        magic, count, name_size = struct.unpack_from('<4sII', blob, 0)
        self.assertEqual(magic, b'PFS0')
        base = s.PFS_HDR + count * s.PFS_ENTRY + name_size
        items = []
        for i in range(count):
            off, size, name_off, res = struct.unpack_from('<qqii', blob, s.PFS_HDR + i * s.PFS_ENTRY)
            self.assertEqual(res, 0)
            at = s.PFS_HDR + count * s.PFS_ENTRY + name_off
            end = blob.index(b'\0', at)
            items.append((blob[at:end].decode(), off, size))
        return [(n, bytes(blob[base + o:base + o + z])) for n, o, z in items]

    def test_layout(self):
        a = s.build_nso(b'\x60\x04\x80\x52', b'A' * 0x10, b'', 0, bytes(0x20))
        b = b'NPD' * 3
        got = dict(self.parse(s.build_pfs0([('main', a), ('main.npdm', b)])))
        self.assertEqual(set(got), {'main', 'main.npdm'})
        self.assertEqual(got['main'], a)
        self.assertEqual(got['main.npdm'], b)
        self.assertEqual(s.build_pfs0([('main', a)])[:3], b'PFS')


class SynthGame(unittest.TestCase):
    """The fake game built right into this work dir; then the chain: NSP -> NCA3."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='synth_test_'))
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        s.write_all(self.dir, 0x5EED5EED)
        text = (self.dir / 'prod.keys').read_text()
        keys = dict(line.split(' = ') for line in text.splitlines() if line and not line.startswith('#'))
        self.kaek = bytes.fromhex(keys['key_area_key_application_00'])
        self.header_key = bytes.fromhex(keys['header_key'])

    def parse(self, blob):
        magic, count, name_size = struct.unpack_from('<4sII', blob, 0)
        assert magic == b'PFS0', magic
        base = s.PFS_HDR + count * s.PFS_ENTRY + name_size
        items = []
        for i in range(count):
            off, size, name_off, res = struct.unpack_from('<qqii', blob, s.PFS_HDR + i * s.PFS_ENTRY)
            at = s.PFS_HDR + count * s.PFS_ENTRY + name_off
            end = blob.index(b'\0', at)
            name = blob[at:end].decode()
            items.append((name, bytes(blob[base + off:base + off + size])))
        return items

    def decrypted_header(self, nca):
        return b''.join(s.xts_decrypt(nca[j * s.SECTOR:(j + 1) * s.SECTOR], self.header_key[:16],
                                      self.header_key[16:], j) for j in range(s.HEADER_SIZE // s.SECTOR))

    def test_nca_and_section_domain(self):
        for nsp in ('base.nsp', 'update.nsp'):
            items = self.parse((self.dir / nsp).read_bytes())
            self.assertEqual(sum(1 for n, _ in items if n.endswith('.nca')), 1)
            nca = [d for n, d in items if n.endswith('.nca')][0]
            h = self.decrypted_header(nca)
            self.assertEqual(h[0x200:0x204], b'NCA3')
            self.assertEqual(h[0x205], s.CONTENT_TYPE_PROGRAM)
            self.assertEqual(struct.unpack_from('<Q', h, 0x210)[0], s.TITLE_ID)
            self.assertEqual(struct.unpack_from('<I', h, 0x208)[0], len(nca))
            start, end, enabled = struct.unpack_from('<iii', h, s.ENTRIES_OFF)
            self.assertEqual(enabled, 1)
            fs = h[s.FS_HEADERS_OFF:s.FS_HEADERS_OFF + 0x200]
            self.assertEqual(hashlib.sha256(fs).digest(), h[s.FS_HASH_OFF:s.FS_HASH_OFF + 0x20])
            content_key = s._dec(self.kaek, h[s.KEY_AREA_OFF + s.CTR_SLOT * 0x10:s.KEY_AREA_OFF + (s.CTR_SLOT + 1) * 0x10])
            upper = struct.unpack_from('<Q', fs, 320)[0]
            raw = s.ctr_encrypt(nca[start * s.SECTOR:end * s.SECTOR], content_key, upper, start * s.SECTOR)
            table_len, data_len = struct.unpack_from('<qq', fs, 64)
            table, data = raw[:table_len], raw[table_len:table_len + data_len]
            self.assertEqual(hashlib.sha256(table).digest(), fs[8:40])
            unit = s.HASH_UNIT
            for i in range(0, len(data), unit):
                self.assertEqual(hashlib.sha256(data[i:i + unit]).digest(), table[(i // unit) * 32:(i // unit + 1) * 32])
            items = self.parse(data)
            self.assertEqual([n for n, _ in items], ['main'])
            self.assertEqual(items[0][1][:4], b'NSO0')

    def test_base_and_update_share_id_differ_code(self):
        headers = {}
        for nsp in ('base.nsp', 'update.nsp'):
            nca = [d for n, d in self.parse((self.dir / nsp).read_bytes()) if n.endswith('.nca')][0]
            headers[nsp] = self.decrypted_header(nca)
        hb = headers['base.nsp']
        hu = headers['update.nsp']
        # the title id matches (the reader requires it); the code section does not (a different version)
        self.assertEqual(struct.unpack_from('<Q', hb, 0x210)[0], struct.unpack_from('<Q', hu, 0x210)[0])
        self.assertNotEqual(hb[s.FS_HASH_OFF:s.FS_HASH_OFF + 0x20], hu[s.FS_HASH_OFF:s.FS_HASH_OFF + 0x20])


class NxExtract(unittest.TestCase):
    """The whole chain through the vendored nx_extract: the NSPs pass
    ReadKeyFile/OpenNsp/header decryption, and the ExeFS comes out byte for byte.
    Needs work/nx_extract built (dotnet build dm/tools/nx_extract in nxnative-src)."""

    @classmethod
    def setUpClass(cls):
        cls.nx = REPO / 'work' / 'nx_extract' / 'nx_extract'
        if not cls.nx.is_file():
            raise unittest.SkipTest('work/nx_extract is not built')

    def setUp(self):
        self.out = Path(tempfile.mkdtemp(prefix='nx_extract_out_'))
        self.addCleanup(shutil.rmtree, self.out, ignore_errors=True)
        s.write_all(REPO / 'work' / 'synth', s.DEFAULT_SEED)

    def run_nx_extract(self, update=None, extra=()):
        args = [str(self.nx), str(REPO / 'work' / 'synth' / 'prod.keys'),
                str(REPO / 'work' / 'synth' / 'base.nsp'), update or '-', str(self.out)]
        return subprocess.run(args + list(extra), capture_output=True, text=True, timeout=120)

    def test_base_only(self):
        _, _, base, _ = s.synth_sequence(s.DEFAULT_SEED)
        p = self.run_nx_extract()
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn(f'title {s.TITLE_ID:016x}', p.stdout)
        self.assertIn('main build id', p.stdout)
        self.assertIn('program has no data section: empty RomFS', p.stdout)
        self.assertEqual((self.out / 'ExeFS' / 'main').read_bytes(), base)

    def test_update_merge(self):
        _, _, _, update = s.synth_sequence(s.DEFAULT_SEED)
        p = self.run_nx_extract(str(REPO / 'work' / 'synth' / 'update.nsp'))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn(f'title {s.TITLE_ID:016x}, update {s.TITLE_ID:016x}', p.stdout)
        self.assertIn('main build id', p.stdout)
        # the merged ExeFS is the update's module, byte for byte with what we built
        self.assertEqual((self.out / 'ExeFS' / 'main').read_bytes(), update)


if __name__ == '__main__':
    unittest.main()
