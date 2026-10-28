#!/usr/bin/env python3
"""Synthetic inputs for the file-format tools: a throwaway prod.keys and NSPs of a fake game.

The real game files (MK8D base NSP with both updates) can only come from a console
(docs/plan.md). To exercise the loader, nso.py and the extraction side before they
exist, this builds the same containers end to end against LibHac's own acceptance
rules (a decompiled copy lives at work/libhac-src; Nca.cs, NcaHeader.cs and
PartitionFileSystemFormat.cs are the references for every offset below), with keys
that never touch real material:

  · NCA3 header (0xC00): AES-128-XTS with the keyset's header_key, 512-byte
    sectors, sector = its index (NcaHeader.DecryptHeader). Two things are
    load-bearing for nx_extract: the FS header hashes (GetFsHeader) and, behind
    the sections, the key area.
  · Sections: AES-128-CTR with the decrypted key area slot 2 (NcaKeyType.AesCtr);
    counter bytes 0..7 = the FS header's Counter (CreateCounter), 8..15 =
    (section offset + rel) /16 (Aes128CtrStorage.UpdateCounter).
  · Section hash tree: the section's FS header carries a master hash over the
    SHA-256 table (one digest per 0x10000 block of the file system); the table
    directly precedes the file system data (InitIvfcForPartitionFs).
  · No ticket: the key area holds plain content keys, not title-key ones.
  · prod.keys: only header_key and key_area_key_application_00. Deliberately no
    master_key_00 and no *_source keys: LibHac ships the real source constants
    (DefaultKeySet) and re-derives the finals from them (KeyDerivation), so a
    loaded master_key_00 would clobber our finals. With the master keys zeroed
    the derivation steps skip and the two throwaway keys survive.

Writes work/synth/{prod.keys, base.nsp, update.nsp} by default (gitignored); every
byte is pseudorandom from a fixed seed, so a CI test can pin the artifacts.
"""
import hashlib
import random
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PAGE = 0x1000
SECTOR = 0x200            # header XTS sector size
FIRST_SECTION = 0x4000    # where every section begins inside its NCA
HASH_UNIT = 0x10000       # section hash-tree unit
PFS_ALIGN = 0x200

HEADER_SIZE = 0xC00
ENTRIES_OFF = 0x240
FS_HASH_OFF = 0x280
KEY_AREA_OFF = 0x300
FS_HEADERS_OFF = 0x400

NCA3 = 3                  # the only version LibHac opens from an NSP
CONTENT_TYPE_PROGRAM = 0
DIST_TYPE_DOWNLOAD = 1
FS_FORMAT_PFS0 = 1
FS_HASH_SHA256 = 2
FS_ENCRYPT_CTR = 3
GF_REDUCTION = 0x87       # x^128 + x^7 + x^2 + x + 1, the tweak's wrap term
CTR_SLOT = 2              # NcaKeyType.AesCtr
DEFAULT_SEED = 0x5EED5EED

# --- AES core (a Cipher call per block: the data is tiny, speed is a non-goal) ---------


def _enc(key, block):
    return Cipher(algorithms.AES(key), modes.ECB()).encryptor().update(bytes(block))

def _dec(key, block):
    return Cipher(algorithms.AES(key), modes.ECB()).decryptor().update(bytes(block))

def _mul_tweak(i):
    """AesXtsMode.Gf128Mul: x*2 reading the tweak as a little-endian 128-bit value,
    0x87 folded in when the top bit walks out."""
    return ((i << 1) & ((1 << 128) - 1)) ^ (GF_REDUCTION if (i >> 127) & 1 else 0)

def _tweak_init(key2, sector):
    """Aes128XtsTransform.FillArrayFromSector: big-endian sector in bytes 8..15,
    encrypted once with the tweak key."""
    return int.from_bytes(_enc(key2, b'\0' * 8 + sector.to_bytes(8, 'big')), 'little')

def _xts(data, key1, key2, sector, step):
    """One TransformBlock stretch = one 512-byte sector. key1 data / key2 tweak."""
    assert len(data) % 16 == 0
    tweak = _tweak_init(key2, sector)
    out = bytearray()
    for i in range(0, len(data), 16):
        t = tweak.to_bytes(16, 'little')
        out += bytes(c ^ d for c, d in zip(step(key1, bytes(c ^ d for c, d in zip(data[i:i + 16], t))), t))
        tweak = _mul_tweak(tweak)
    return bytes(out)

def xts_encrypt(data, key1, key2, sector):
    return _xts(data, key1, key2, sector, _enc)

def xts_decrypt(data, key1, key2, sector):
    return _xts(data, key1, key2, sector, _dec)

def ctr_encrypt(data, key, counter_hi, first_off):
    """AES-128-CTR per the reader: IV bytes 0..7 = counter_hi (an int) big endian,
    8..15 = (first_off + rel)/16 big endian (Nca.OpenAesCtrStorage)."""
    assert len(data) % 16 == 0
    out = bytearray()
    for pos in range(0, len(data), 16):
        ctr = counter_hi.to_bytes(8, 'big') + ((first_off + pos) // 16).to_bytes(8, 'big')
        out += bytes(c ^ d for c, d in zip(data[pos:pos + 16], _enc(key, ctr)))
    return bytes(out)

def _align(n, a):
    return (n + a - 1) // a * a

def _page_up(n):
    return _align(n, PAGE)

# --- NSO (ExeFS modules), constrained to what tools/nso.py accepts ---------------------

NSO_HDR = 0x100

def build_nso(text, ro, data, bss, build_id):
    """NSO0 with the flags clear: no compression, no segment hashes. load() requires
    text at 0 in memory, ro and data after it on whole pages, nothing overlapping;
    the file's segments follow right after the header."""
    assert len(build_id) == 0x20
    mos = (0, _page_up(len(text)), _page_up(len(text)) + _page_up(len(ro)))
    assert mos[0] == 0 and mos[1] >= len(text) and mos[2] >= mos[1] + len(ro)   # load() checks these
    header = bytearray(NSO_HDR)
    header[0:4] = b'NSO0'
    struct.pack_into('<I', header, 0xC, 0)                             # flags: plain segments
    at = NSO_HDR
    for i, (sz, mo) in enumerate(zip((len(text), len(ro), len(data)), mos)):
        struct.pack_into('<III', header, 0x10 + i * 0x10, at, mo, sz)
        at += sz
    struct.pack_into('<I', header, 0x3C, bss)
    header[0x40:0x60] = build_id
    struct.pack_into('<III', header, 0x60, len(text), len(ro), len(data))
    return bytes(header) + text + ro + data

# --- PFS0 (the NSP container, and the NCA Code sections) -------------------------------

PFS_HDR = 0x10
PFS_ENTRY = 0x18   # int64 offset (rel to metadata end), int64 size, int32 name off, int32

def _pfs_entry_data(files):
    """(entries, name table, data base at the metadata end, per-file data offsets)"""
    names = bytearray()
    name_offs = []
    for name, _ in files:
        name_offs.append(len(names))
        names += name.encode() + b'\0' + b'\0' * ((4 - (len(name) + 1) % 4) % 4)
    entries = bytearray()
    data_base = PFS_HDR + len(files) * PFS_ENTRY + len(names)
    offs = []
    pos = 0
    for (name, d), name_off in zip(files, name_offs):
        entries += struct.pack('<qqii', pos, len(d), name_off, 0)
        offs.append(pos)
        pos += _align(len(d), PFS_ALIGN)
    return bytes(entries), bytes(names), data_base, offs

def build_pfs0(files):
    """files: ordered [(name, bytes)] -> the container bytes. Entry offsets are
    relative to the end of the metadata (PartitionFileSystemCore adds
    _metaDataSize), which is also where every file's data starts, 0x200-aligned."""
    entries, names, data_base, offs = _pfs_entry_data(files)
    out = bytearray(b'PFS0' + struct.pack('<II', len(files), len(names)) + b'\0' * 4 + entries + names)
    for (_, d), pos in zip(files, offs):
        assert len(out) == data_base + pos
        out += d
        out += b'\0' * (_align(len(d), PFS_ALIGN) - len(d))
    return bytes(out)

# --- NCA3 ------------------------------------------------------------------------------

def build_fs_header_pfs(hash_table_len, data_len, block_size=HASH_UNIT, counter=(1, 0)):
    """FS header of a Code section: PFS0 / SHA-256 / CTR. Layout per NcaFsHeader:
    +0x08 the SHA-256 struct (master hash, block size, levels), +0x100 the patch
    info (zeros: not a patch section), +0x140 the CTR counter pair."""
    fs = bytearray(0x200)
    struct.pack_into('<H', fs, 0, 2)                    # header version
    fs[2] = FS_FORMAT_PFS0
    fs[3] = FS_HASH_SHA256
    fs[4] = FS_ENCRYPT_CTR
    struct.pack_into('<i', fs, 40, block_size)          # block size of the hashed data
    struct.pack_into('<i', fs, 44, 2)                   # level count (InitIvfcForPartitionFs)
    struct.pack_into('<qq', fs, 48, 0, hash_table_len)              # level 0: the table
    struct.pack_into('<qq', fs, 64, hash_table_len, data_len)       # level 1: the data
    struct.pack_into('<ii', fs, 320, counter[0], counter[1])        # CounterType / Version
    return fs

def build_pfs_section(pfs):
    """(fs_header, section data) for a Code section: SHA-256 table then the PFS0.
    The master hash covers the whole table (a single huge block in the tree)."""
    table = b''.join(hashlib.sha256(pfs[i:i + HASH_UNIT]).digest() for i in range(0, len(pfs), HASH_UNIT))
    fs = build_fs_header_pfs(len(table), len(pfs))
    fs[8:40] = hashlib.sha256(table).digest()
    return bytes(fs), table + pfs

def build_nca3(sections, title_id, content_type, header_key, kaek, keygen=0,
               key_index=0, distribution=DIST_TYPE_DOWNLOAD, sdk_version=0x00040400,
               content_index=0):
    """One NCA3 file. sections = [(fs_header, plain data)]; every section starts at
    FIRST_SECTION or right after the previous one, on sector boundaries. Keys:
    header_key for the XTS header, kaek hides the one section content key (slot 2,
    shared by every section, as on a real NCA). Slots 0, 1, 3 are noise."""
    assert len(header_key) == 32 and len(kaek) == 16
    assert 0 <= keygen <= 2 and 0 <= content_type <= 5 and title_id > 0

    key_area = bytearray(0x100)
    for slot in (0, 1, 3):
        key_area[slot * 0x10:slot * 0x10 + 0x10] = rng_rand(int.from_bytes(kaek[slot * 2:slot * 2 + 2], 'big') or 1, 16)
    content_key = rng_rand(int.from_bytes(kaek[14:], 'big'), 16)
    key_area[CTR_SLOT * 0x10:CTR_SLOT * 0x10 + 0x10] = _enc(kaek, content_key)

    placements = []
    fs_hashes = []
    entries = bytearray()
    at = FIRST_SECTION
    for fs, plain in sections:
        assert at % SECTOR == 0 and at >= HEADER_SIZE
        end = _align(at + len(plain), SECTOR)
        entries += struct.pack('<iii', at // SECTOR, end // SECTOR, 1) + b'\0' * 4
        fs_hashes.append(hashlib.sha256(fs).digest())
        placements.append((at, fs, plain))
        at = end
    file_len = at

    header = bytearray(HEADER_SIZE)
    header[0x200:0x204] = b'NCA3'
    header[0x204] = distribution
    header[0x205] = content_type
    header[0x206] = keygen
    header[0x207] = key_index
    struct.pack_into('<q', header, 0x208, file_len)
    struct.pack_into('<Q', header, 0x210, title_id)
    struct.pack_into('<i', header, 0x218, content_index)
    struct.pack_into('<I', header, 0x21C, sdk_version)
    for i in range(len(placements)):
        header[ENTRIES_OFF + i * 0x10:ENTRIES_OFF + (i + 1) * 0x10] = entries[i * 0x10:(i + 1) * 0x10]
        header[FS_HASH_OFF + i * 0x20:FS_HASH_OFF + (i + 1) * 0x20] = fs_hashes[i]
        header[FS_HEADERS_OFF + i * 0x200:FS_HEADERS_OFF + (i + 1) * 0x200] = placements[i][1]
    header[KEY_AREA_OFF:KEY_AREA_OFF + 0x100] = bytes(key_area)

    out = bytearray()
    for j in range(HEADER_SIZE // SECTOR):
        out += xts_encrypt(bytes(header[j * SECTOR:(j + 1) * SECTOR]), header_key[:16], header_key[16:], j)
    out += b'\0' * (FIRST_SECTION - HEADER_SIZE)
    for at_, fs, plain in placements:
        assert len(out) == at_                           # sections are contiguous as built
        upper = struct.unpack_from('<Q', fs, 320)[0]
        out += ctr_encrypt(plain, content_key, upper, at_)
        out += b'\0' * (_align(len(plain), SECTOR) - len(plain))
    assert len(out) == file_len
    return bytes(out)

def rng_rand(seed, n):
    return random.Random(seed).randbytes(n)          # no key material: just any pseudo bytes

# --- The fake game ---------------------------------------------------------------------

TITLE_ID = 0x51595354484B21          # 'SYSTHK!' in ascii; no game uses it

def _module(rng, kind):
    """A tiny aarch64 module: mov w0, <kind>; svc #0; ret. One page per segment.
    The build ID is what nx_extract prints for it (first 0x10 of this)."""
    ins = b'\x60\x04\x80\x52' if kind == 'base' else b'\x80\x04\x80\x52'   # mov w0, 0x23 / 0x24
    text = (ins + b'\x01\x00\x00\xd4' + b'\xc0\x03\x5f\xd6').ljust(0x1000, b'\0')   # svc #0; ret
    ro = (b'(synthetic %s module)\0' % kind.encode()).ljust(0x1000, b'\0')
    data = b'\0' * 0x1000
    return build_nso(text, ro, data, 0x4000, rng.randbytes(0x20))

def make_game_nca(header_key, kaek, module):
    pfs = build_pfs0([('main', module)])
    assert len(pfs) % 16 == 0
    return build_nca3([build_pfs_section(pfs)], TITLE_ID, CONTENT_TYPE_PROGRAM,
                      header_key, kaek)

def synth_sequence(seed):
    """The exact byte stream write_all generates, so tests can expect outputs:
    (header_key, kaek, base module, update module) for one seed."""
    rng = random.Random(seed)
    header_key = rng.randbytes(32)
    kaek = rng.randbytes(16)
    return header_key, kaek, _module(rng, 'base'), _module(rng, 'patch')

def write_all(out, seed=DEFAULT_SEED):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    header_key, kaek, base_module, update_module = synth_sequence(seed)
    (out / 'prod.keys').write_text(
        '# synthetic keyset for the pipeline tests (docs/plan.md); not from any console\n'
        f'header_key = {header_key.hex()}\n'
        f'key_area_key_application_00 = {kaek.hex()}\n')
    for name, module in (('base.nsp', base_module), ('update.nsp', update_module)):
        nca = make_game_nca(header_key, kaek, module)
        (out / name).write_bytes(build_pfs0([(content_id(nca) + '.nca', nca)]))
    print(f'wrote {out}/prod.keys base.nsp update.nsp (seed {seed:#x})')

def content_id(nca):
    """The .nca file name inside an NSP: hex of what would be the content's first id."""
    return hashlib.sha256(nca).hexdigest()[:32]

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'work/synth'
    seed = int(sys.argv[2], 0) if len(sys.argv) > 2 else DEFAULT_SEED
    write_all(out, seed)

if __name__ == '__main__':
    sys.exit(main())
