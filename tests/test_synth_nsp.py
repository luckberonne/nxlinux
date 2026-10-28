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
        self.assertIn('RomFS: 3 files', p.stdout)
        self.assertEqual((self.out / 'ExeFS' / 'main').read_bytes(), base)
        self.assertEqual((self.out / 'RomFS' / 'data' / 'config.txt').read_bytes(),
                         b'synthetic romfs config\nvariant: base\n')
        self.assertEqual((self.out / 'RomFS' / 'assets' / 'greeting.txt').read_bytes(),
                         b'hello from the synthetic RomFS (base)\n')
        self.assertEqual((self.out / 'RomFS' / 'assets' / 'tile.dat').read_bytes(),
                         s.rng_rand(0x7171, 0x4800))

    def test_update_merge(self):
        _, _, _, update = s.synth_sequence(s.DEFAULT_SEED)
        p = self.run_nx_extract(str(REPO / 'work' / 'synth' / 'update.nsp'))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn(f'title {s.TITLE_ID:016x}, update {s.TITLE_ID:016x}', p.stdout)
        self.assertIn('main build id', p.stdout)
        # the merged ExeFS is the update's module, byte for byte with what we built
        self.assertEqual((self.out / 'ExeFS' / 'main').read_bytes(), update)
        # the merged RomFS is the update's (no BKTR: the patch owns its whole data section)
        self.assertEqual((self.out / 'RomFS' / 'data' / 'config.txt').read_bytes(),
                         b'synthetic romfs config\nvariant: update\n')
        self.assertEqual((self.out / 'RomFS' / 'data' / 'extra.txt').read_bytes(), b'only in the update\n')

    def test_manifest(self):
        p = self.run_nx_extract(extra=('--manifest',))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('RomFS 3 files', p.stdout)
        rows = (self.out / 'romfs_manifest.tsv').read_text().splitlines()
        paths = [row.split('\t')[0] for row in rows[1:]]
        self.assertEqual(sorted(paths), ['/assets/greeting.txt', '/assets/tile.dat',
                                         '/data/config.txt'])
        tile = [row for row in rows if 'tile.dat' in row][0]
        self.assertEqual(tile.split('\t')[1], str(0x4800))

    def test_romfs_image(self):
        p = self.run_nx_extract(extra=('--romfs-image',))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('image with 6 entries', p.stdout)   # root + 2 dirs + 3 files
        self.assertFalse((self.out / 'RomFS').exists())
        blob = (self.out / 'RomFS.img').read_bytes()
        magic, off, size, count = struct.unpack_from('<8sQQQ', blob, 0)
        self.assertEqual(magic, b'DMROMFS1')
        index = blob[off:off + size]
        names = []
        for k in range(count):
            a, b, name_off, name_len, kind, pad = struct.unpack_from('<QQIHBB', index, k * 24)
            name = index[count * 24 + name_off:count * 24 + name_off + name_len]
            if kind == 1 and name == b'config.txt':
                self.assertEqual(blob[a:a + b], b'synthetic romfs config\nvariant: base\n')
            names.append(name)
        self.assertEqual(set(names), {b'', b'assets', b'data', b'greeting.txt',
                                      b'config.txt', b'tile.dat'})

    def test_no_romfs(self):
        dir2 = Path(tempfile.mkdtemp(prefix='synth_test_no_romfs_'))
        self.addCleanup(shutil.rmtree, dir2, ignore_errors=True)
        s.write_all(dir2, s.DEFAULT_SEED, with_romfs=False)
        p = subprocess.run([str(self.nx), str(dir2 / 'prod.keys'), str(dir2 / 'base.nsp'),
                            '-', str(self.out)], capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('program has no data section: empty RomFS', p.stdout)

    def test_damaged_romfs(self):
        dir2 = Path(tempfile.mkdtemp(prefix='synth_test_damaged_'))
        self.addCleanup(shutil.rmtree, dir2, ignore_errors=True)
        s.write_all(dir2, s.DEFAULT_SEED)
        keys = dict(line.split(' = ') for line in (dir2 / 'prod.keys').read_text().splitlines()
                    if line and not line.startswith('#'))
        header_key = bytes.fromhex(keys['header_key'])
        kaek = bytes.fromhex(keys['key_area_key_application_00'])
        nsp_path = dir2 / 'base.nsp'
        blob = bytearray(nsp_path.read_bytes())
        nca_at, nca = self.nca_at(blob)
        header = b''.join(s.xts_decrypt(nca[j * s.SECTOR:(j + 1) * s.SECTOR], header_key[:16],
                                        header_key[16:], j) for j in range(s.HEADER_SIZE // s.SECTOR))
        content_key = s._dec(kaek, header[s.KEY_AREA_OFF + s.CTR_SLOT * 0x10:
                                          s.KEY_AREA_OFF + (s.CTR_SLOT + 1) * 0x10])
        start, end, _ = struct.unpack_from('<iii', header, s.ENTRIES_OFF + 0x10)   # section 1: the data
        fs = header[s.FS_HEADERS_OFF + 0x200:s.FS_HEADERS_OFF + 0x400]
        upper = struct.unpack_from('<Q', fs, 320)[0]
        plain = s.ctr_encrypt(nca[start * s.SECTOR:end * s.SECTOR], content_key, upper, start * s.SECTOR)
        r2_off = struct.unpack_from('<q', fs, 72)[0]                              # region 2: the image
        data_at, _ = self.romfs_file_slice(plain[r2_off:r2_off + 0x10000], b'tile.dat')
        # flip one ciphertext byte inside tile.dat's second 0x4000 block (block 0
        # also holds the image header and tables, and every read verifies its
        # sector: there the failure would escape inside the RomFS open rather
        # than surface on the file read it belongs to)
        at = nca_at + start * s.SECTOR + r2_off + data_at + 0x4000 + 0x40
        blob[at] ^= 0xFF
        nsp_path.write_bytes(bytes(blob))
        # a clean run: the hash check fails on the touched data block and stops everything
        p = self.run_raw(dir2)
        self.assertEqual(p.returncode, 3, p.stdout + p.stderr)
        self.assertIn('[RED] hash check failed: /assets/tile.dat', p.stdout)
        self.assertFalse((self.out / 'RomFS' / 'assets' / 'tile.dat').exists())
        # --allow-damaged: the file is written as read, flipped byte included
        p = self.run_raw(dir2, extra=('--allow-damaged',))
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        expect = bytearray(s.rng_rand(0x7171, 0x4800))
        expect[0x4040] ^= 0xFF
        self.assertEqual((self.out / 'RomFS' / 'assets' / 'tile.dat').read_bytes(), bytes(expect))

    def run_raw(self, synth_dir, extra=()):
        args = [str(self.nx), str(synth_dir / 'prod.keys'), str(synth_dir / 'base.nsp'), '-',
                str(self.out)] + list(extra)
        return subprocess.run(args, capture_output=True, text=True, timeout=120)

    @staticmethod
    def nca_at(blob):
        """(offset within the NSP, bytes) of the one .nca inside an NSP."""
        magic, count, name_size = struct.unpack_from('<4sII', blob, 0)
        assert magic == b'PFS0'
        base = s.PFS_HDR + count * s.PFS_ENTRY + name_size
        for i in range(count):
            off, size, name_off, res = struct.unpack_from('<qqii', blob, s.PFS_HDR + i * s.PFS_ENTRY)
            at = s.PFS_HDR + count * s.PFS_ENTRY + name_off
            name = blob[at:blob.index(b'\0', at)]
            if name.endswith(b'.nca'):
                return base + off, bytes(blob[base + off:base + off + size])
        raise AssertionError('no .nca in the NSP')

    @staticmethod
    def romfs_file_slice(img, name):
        """(position, size) of a file's bytes inside a RomFS image, per its own
        header and file table (the walk the reader does over the entry rows)."""
        dh, dsz, dm, msz, fh, fhs, fm, fms, data_off = struct.unpack_from('<9Q', img, 8)
        pos = 0
        while pos < fms:
            parent, sib, foff, flen, nxt, klen = struct.unpack_from('<IiQQiI', img, fm + pos)
            if img[fm + pos + 32:fm + pos + 32 + klen] == name:
                return data_off + foff, flen
            pos += 32 + klen
            pos += -pos % 4
        raise AssertionError(f'{name} not in the romfs')


class Romfs(unittest.TestCase):
    """The RomFS image and its IVFC section against the reader's own semantics:
    re-parse the image, walk the buckets/chains as HierarchicalRomFileTable does,
    and rebuild the hash chain from region digests."""

    def parse(self, img):
        self.assertEqual(struct.unpack_from('<I', img, 0)[0], 0x50)
        return struct.unpack_from('<9Q', img, 8)

    def table_dirs(self, img):
        """Dir table rows in order: (offset, parent, name, next) + row positions."""
        dh, dsz, dm, msz, fh, fhs, fm, fms, data_off = self.parse(img)
        rows, pos = [], 0
        while pos < msz:
            parent, sib, cdir, cfile, nxt, klen = struct.unpack_from('<IiiiiI', img, dm + pos)
            rows.append((pos, parent, bytes(img[dm + pos + 24:dm + pos + 24 + klen]), nxt))
            pos += 24 + klen
            pos += -pos % 4
        return rows

    def table_files(self, img):
        """{name: (data position, size)} over the file meta table rows."""
        dh, dsz, dm, msz, fh, fhs, fm, fms, data_off = self.parse(img)
        out, pos = {}, 0
        while pos < fms:
            parent, sib, foff, flen, nxt, klen = struct.unpack_from('<IiQQiI', img, fm + pos)
            out[bytes(img[fm + pos + 32:fm + pos + 32 + klen])] = (data_off + foff, flen)
            pos += 32 + klen
            pos += -pos % 4
        return out

    def test_image_walk(self):
        """Root in a bucket under key (0, b''), then siblings and buckets agree with
        what the lookups and the enumeration walk."""
        img = s.make_romfs('base')
        dh, dsz, dm, msz, fh, fhs, fm, fms, data_off = self.parse(img)
        d_buckets = struct.unpack_from('<%di' % (dsz // 4), img, dh)
        self.assertEqual(data_off % 16, 0)
        root_parent, root_sib, root_dirs, root_files, root_next, root_klen = \
            struct.unpack_from('<IiiiiI', img, dm)
        self.assertEqual((root_parent, root_sib, root_klen), (0, -1, 0))
        # every entry is reachable through its bucket chain (the lookup walk)
        rows = self.table_dirs(img)
        walked = set()
        for i in range(dsz // 4):
            e = d_buckets[i]
            while e != -1:
                self.assertNotIn(e, walked)
                walked.add(e)
                nxt = struct.unpack_from('<i', img, dm + e + 16)[0]
                klen = struct.unpack_from('<I', img, dm + e + 20)[0]
                expected = s._romfs_name_hash(struct.unpack_from('<I', img, dm + e)[0],
                                              img[dm + e + 24:dm + e + 24 + klen]) % len(d_buckets)
                self.assertEqual(i, expected)
                e = nxt
        self.assertEqual(walked, {pos for pos, _, _, _ in rows})
        # the enumeration walk: root's children, then each dir's file list
        files = self.table_files(img)
        self.assertEqual(len(files), 3)
        config_at, config_len = files[b'config.txt']
        self.assertEqual(img[config_at:config_at + config_len],
                         b'synthetic romfs config\nvariant: base\n')
        tile_at, tile_len = files[b'tile.dat']
        self.assertEqual(img[tile_at:tile_at + tile_len], s.rng_rand(0x7171, 0x4800))
        self.assertEqual(tile_len, 0x4800)                       # spans several 0x4000 blocks

    def test_section_hash_chain(self):
        """The IVFC tree of build_romfs_section recomputes from the section itself."""
        img = s.make_romfs('update')
        fs, section = s.build_romfs_section(img)
        self.assertEqual(fs[0:2], b'\x02\0')                     # header version
        self.assertEqual(fs[2], s.FS_FORMAT_ROMFS)
        self.assertEqual(fs[3], s.FS_HASH_IVFC)
        self.assertEqual(fs[4], s.FS_ENCRYPT_CTR)
        magic, version, master_size, levels = struct.unpack_from('<4siii', fs, 8)
        self.assertEqual((magic, version, levels), (b'IVFC', 0x10000, 4))
        self.assertEqual(master_size, 32)
        regions = [struct.unpack_from('<qqii', fs, 24 + j * 24) for j in range(3)]
        # every level's hashes recompute over the next-lower level's bytes
        r0 = section[:regions[0][1]]
        r1 = section[regions[1][0]:regions[1][0] + regions[1][1]]
        data = section[regions[2][0]:regions[2][0] + regions[2][1]]
        self.assertEqual(fs[200:232], s._block_hashes(r0, 1 << regions[0][2]))
        self.assertEqual(s._block_hashes(r1, 1 << regions[1][2]), r0)
        self.assertEqual(s._block_hashes(data, 1 << regions[2][2]), r1)
        self.assertEqual(data, img)                              # the data level is the image
        # and the sizes: region 0 holds one digest per 0x1000 of region 1
        self.assertEqual(regions[0][1] % 32, 0)
        self.assertEqual(regions[0][1], ((regions[1][1] + 0xFFF) // 0x1000) * 32)

    def test_update_differs_from_base_only_where_expected(self):
        base, update = s.make_romfs('base'), s.make_romfs('update')
        fb, fu = self.table_files(base), self.table_files(update)
        self.assertEqual(len(fu), 4)                             # + data/extra.txt
        eb = base[fb[b'config.txt'][0]:fb[b'config.txt'][0] + fb[b'config.txt'][1]]
        eu = update[fu[b'config.txt'][0]:fu[b'config.txt'][0] + fu[b'config.txt'][1]]
        self.assertNotEqual(eb, eu)                              # config changed
        ct = update[fu[b'tile.dat'][0]:fu[b'tile.dat'][0] + fu[b'tile.dat'][1]]
        cb = base[fb[b'tile.dat'][0]:fb[b'tile.dat'][0] + fb[b'tile.dat'][1]]
        self.assertEqual(ct, cb)                                 # tile.dat identical


if __name__ == '__main__':
    unittest.main()
