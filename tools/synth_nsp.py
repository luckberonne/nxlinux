#!/usr/bin/env python3
"""Synthetic inputs for the file-format tools: a throwaway prod.keys and NSPs of a fake game.

The real game files (MK8D base NSP with both updates) can only come from a console
(docs/plan.md). To exercise the loader, nso.py and the extraction side before they
exist, this builds the same containers end to end against LibHac's own acceptance
rules (a decompiled copy lives at work/libhac-src; Nca.cs, NcaHeader.cs and
PartitionFileSystemFormat.cs are the references for every offset below), with keys
that never touch real material:

  · NCA3 header (0xC00): AES-128-XTS with the keyset's header_key, 512-byte
    sectors, sector = its index (NcaHeader.DecryptHeader). Only two things are
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
    (DefaultKeySet) and re-derives the finals from them (KeyType.Derive*), so a
    loaded master_key_00 would clobber our finals. With the master keys zeroed
    the derivation steps skip and the two throwaway keys survive.

Writes work/synth/{prod.keys, base.nsp, update.nsp} by default (gitignored); every
byte is pseudorandom from a fixed seed, so a CI test can pin the artifacts.
"""
import hashlib
import random
import struct
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

# --- AES core (one Cipher call per block: the data is tiny, speed is a non-goal) -------


def _ecb(key, block):
    return Cipher(algorithms.AES(key), modes.ECB()).encryptor().update(bytes(block))

def _mul_tweak(i):
    """AesXtsMode.Gf128Mul: x*2 reading the tweak as a little-endian 128-bit value,
    0x87 folded in when the top bit walks out."""
    return ((i << 1) & ((1 << 128) - 1)) ^ (GF_REDUCTION if (i >> 127) & 1 else 0)

def _tweak_init(key2, sector):
    """Aes128XtsTransform.FillArrayFromSector: big-endian sector in bytes 8..15,
    encrypted once with the tweak key."""
    return int.from_bytes(_ecb(key2, b'\0' * 8 + sector.to_bytes(8, 'big')), 'little')

def xts_encrypt(data, key1, key2, sector):
    """One TransformBlock stretch = one 512-byte sector. key1 data / key2 tweak."""
    tweak = _tweak_init(key2, sector)
    out = bytearray()
    for i in range(0, len(data), 16):
        t = tweak.to_bytes(16, 'little')
        out += bytes(c ^ d for c, d in zip(_ecb(key1, bytes(c ^ d for c, d in zip(data[i:i + 16], t))), t))
        tweak = _mul_tweak(tweak)
    return bytes(out)

def xts_decrypt(data, key1, key2, sector):
    """Inverse of xts_encrypt (LibHac's DecryptHeader reads back through this)."""
    tweak = _tweak_init(key2, sector)
    out = bytearray()
    for i in range(0, len(data), 16):
        t = tweak.to_bytes(16, 'little')
        out += bytes(a ^ b for a, b in zip(_ecb(key1, bytes(c ^ d for c, d in zip(data[i:i + 16], t))), t))
        tweak = _mul_tweak(tweak)
    return bytes(out)

def ctr_encrypt(data, key, counter_hi, first_off):
    """AES-128-CTR per the reader: IV bytes 0..7 = counter_hi big endian, 8..15 =
    (first_off + rel)/16 big endian (Nca.OpenAesCtrStorage)."""
    out = bytearray()
    for pos in range(0, len(data), 16):
        ctr = counter_hi.to_bytes(8, 'big') + ((first_off + pos) // 16).to_bytes(8, 'big')
        out += bytes(c ^ d for c, d in zip(data[pos:pos + 16], _ecb(key, ctr)))
    return bytes(out)

def _align(n, a):
    return (n + a - 1) // a * a

def _page_up(n):
    return _align(n, PAGE)

# --- NSO (ExeFS modules), constrained to what tools/nso.py accepts ---------------------

NSO_HDR = 0x100

def build_nso(text, ro, data, bss, build_id):
    """NSO0 with the flags clear: no compression, no segment hashes. Load() requires
    text at 0 in memory, ro and data after it on whole pages, nothing overlapping;
    the files' segments follow right after the header."""
    assert len(build_id) == 0x20
    mos = (0, _page_up(len(text)), _page_up(len(text)) + _page_up(len(ro)))
    assert mos[1] >= len(text) and mos[2] >= mos[1] + len(ro)          # load() checks this
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
PFS_ENTRY = 0x18   # int64 offset (rel to metadata end), int64 size, int32 name off, uint32

def build_pfs0(files):
    """files: ordered [(name, bytes)] -> the container bytes. Entry offsets are
    relative to the end of the metadata (PartitionFileSystemCore adds
    _metaDataSize); file data follows it, every file's start 0x200-aligned."""
    names = bytearray()
    for name, _ in files:
        names += name.encode() + b'\0' + b'\0' * ((4 - (len(name) + 1) % 4) % 4)
    entries = bytearray()
    data_base = PFS_HDR + len(files) * PFS_ENTRY + len(names)
    pos = 0
    for name, d in files:
        entries += struct.pack('<qqii', pos, len(d), len(name) and names.index(name.encode() + b'\0'), 0)
        pos += _align(len(d), PFS_ALIGN)
    out = bytearray(b'PFS0' + struct.pack('<II', len(files), len(names)) + b'\0' * 4 + bytes(entries) + bytes(names))
    for (_, d), at in zip(files, [0] * len(files)):
        pass   # replaced below; placeholder removed
    pos = 0
    for name, d in files:
        assert len(out) == data_base + pos
        out += d
        out += b'\0' * (_align(len(d), PFS_ALIGN) - len(d))
        pos += _align(len(d), PFS_ALIGN)
    return bytes(out)

# --- NCA3 ------------------------------------------------------------------------------

def build_fs_header_pfs(hash_table, data_len, block_size=HASH_UNIT, counter=(1, 0)):
    """FS header of a Code section: PFS0 / SHA-256 / CTR. Layout per NcaFsHeader
    (+0x08 the SHA-256 struct, +0x100 patch info, +0x140 the CTR counter)."""
    fs = bytearray(0x200)
    struct.pack_into('<H', fs, 0, 2)                    # header version
    fs[2] = FS_FORMAT_PFS0
    fs[3] = FS_HASH_SHA256
    fs[4] = FS_ENCRYPT_CTR
    fs[8:40] = hashlib.sha256(hash_table).digest()      # master hash covers the table alone
    struct.pack_into('<i', fs, 40, block_size)          # block size of the hashed data
    struct.pack_into('<i', fs, 44, 2)                   # level count (see InitIvfcForPartitionFs)
    struct.pack_into('<qq', fs, 48, 0, len(hash_table))             # level 0: the hash table
    struct.pack_into('<qq', fs, 64, len(hash_table), data_len)      # level 1: the data
    struct.pack_into('<ii', fs, 320, counter[0], counter[1])        # CounterType / CounterVersion
    return bytes(fs)

def build_pfs_section(pfs):
    """(fs_header, section data) for a Code section: SHA-256 table then the PFS0."""
    table = b''.join(hashlib.sha256(pfs.setdefault and c).digest() if False else hashlib.sha256(c).digest()
                     for c in (pfs[i:i + HASH_UNIT] for i in range(0, len(pfs), HASH_UNIT)))
    fs = build_fs_header_pfs(table, len(pfs))
    return fs, table + pfs

def build_nca3(sections, title_id, content_type, keygen=0, key_index=0,
               header_key=b'\0' * 32, kaek=b'\0' * 16, distribution=DIST_TYPE_DOWNLOAD,
               sdk_version=0x00040400, content_index=0, section_gap=SECTOR):
    """One NCA3 file. sections = [(fs_header, plain data)]; none may start before
    FIRST_SECTION. Keys: header_key for the XTS header, kaek hides the content key
    of each section's slot 2. Key area slots 0, 1, 3 are noise."""
    assert len(header_key) == 32 and len(kaek) == 16
    assert title_id > 0 and 0 <= content_type <= 5

    entries = bytearray()
    fs_hashes = []
    key_area = bytearray(0x100)
    key_area[:0x10] = random.Random(int.from_bytes(kaek[:2], 'big')).randbytes(0x10)
    key_area[0x10:0x20] = random.Random(int.from_bytes(kaek[2:4], 'big')).randbytes(0x10)
    content_key = random.Random(int.from_bytes(kaek, 'big')).randbytes(16)
    key_area[CTR_SLOT * 0x10:CTR_SLOT * 0x10 + 0x10] = _ecb(kaek, content_key)
    key_area[0x30:0x40] = random.Random(int.from_bytes(kaek[4:6], 'big')).randbytes(0x10)

    placements = []
    at = FIRST_SECTION
    for fs, plain in sections:
        entry = struct.pack('<iii', at // SECTOR, (at + _align(len(plain), SECTOR)) // SECTOR, 1)
        assert at % SECTOR == 0 and at >= HEADER_SIZE
        entries += entry + b'\0' * 4            # enabled flag as 4 bytes + 4 pad
        fs_hashes.append(hashlib.sha256(fs).digest())
        placements.append((at, fs, plain))
        at += _align(len(plain), SECTOR) + (section_gap if placements[-1][0] != at - _align(len(plain), SECTOR) else 0)
    at -= SECTOR if False else 0
    total = at if False else 0
    total = max(at - FIRST_SECTION, 0)
    file_len = at
    if file_len % SECTOR: file_len += SECTOR - file_len % SECTOR

    header = bytearray(HEADER_SIZE)
    header[0x200:0x204] = b'NCA3'
    header[0x204] = distribution
    header[0x205] = content_type
    header[0x206] = keygen if keygen <= 2 else 2
    header[0x207] = key_index
    struct.pack_into('<Q', header, 0x208, file_len)
    struct.pack_into('<Q', header, 0x210, title_id)
    struct.pack_into('<i', header, 0x218, content_index)
    struct.pack_into('<I', header, 0x21C, sdk_version)
    for i, entry in enumerate((
        b''.join(e for e in "") ,
    )):
        pass
    for i, p in enumerate(placements):
        header[ENTRIES_OFF + i * 0x10:ENTRIES_OFF + (i + 1) * 0x10] = entries[i * 0x14:(i + 1) * 0x14][:0x10][:0x14]
    for i, h in enumerate(fs_hashes):
        header[FS_HASH_OFF + i * 0x20:FS_HASH_OFF + (i + 1) * 0x20] = h
    header[KEY_AREA_OFF:KEY_AREA_OFF + 0x100] = bytes(key_area)
    for i, (at_, fs, _) in enumerate(placements):
        header[FS_HEADERS_OFF + i * 0x200:FS_HEADERS_OFF + (i + 1) * 0x200] = fs

    encrypted = bytearray()
    for j in range(HEADER_SIZE // SECTOR):
        encrypted += xts_encrypt(bytes(header[j * SECTOR:(j + 1) * SECTOR]), header_key[:16], header_key[16:], j)
    out = bytes(encrypted) + b'\0' * (FIRST_SECTION - HEADER_SIZE)
    for at_, fs, plain in placements:
        upper = struct.unpack_from('<Q', fs, 320)[0]
        assert len(out) == at_
        out += ctr_encrypt(plain, content_key, upper, at_)
        pad = _align(len(plain), SECTOR) - len(plain)
        out += b'\0' * pad
        if placements and (at_, fs, plain) is not placements[-1]:
            out += b'\0' * section_gap
    assert len(out) == file_len
    return out

# --- The fake game ---------------------------------------------------------------------

TITLE_ID = 0x51595354484B21          # 'SYSTHK!' as an id no game uses

def _module(rng, kind):
    """A tiny aarch64 module: mov w0, <kind>; svc #0; ret. One page per segment."""
    slot = {'base': b'\x60\x04\x80\x52', 'patch': b'\x80\x04\x80\x52'}[kind]   # mov w0, 0x23 / 0x24
    text = (slot + b'\x01\x00\x00\xd4' + b'\xc0\x03\x5f\xd6').ljust(0x1000, b'\0')
    ro = (b'(synthetic %s module)\0' % kind.encode()).ljust(0x1000, b'\0')
    data = b'\0' * 0x1000
    return build_nso(text, ro, data, 0x4000, rng.randbytes(0x20))

def make_game_nca(rng, kind):
    return build_nca3([build_pfs_section(build_pfs0([('main', _module(rng, kind))]))],
                      TITLE_ID, CONTENT_TYPE_PROGRAM, header_key=_rng_key(rng, 32) )

def _rng_key(rng, n):
    return rng.randbytes(n)

def write_all(out, seed=0x5EED5EED):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(seed)
    keys = {'header_key': _rng_key(rng, 32), 'key_area_key_application_00': _rng_key(rng, 16)}
    (out / 'prod.keys').write_text(
        '# synthetic keyset for the pipeline tests (docs/plan.md); not from any console\n'
        + ''.join(f'{name} = {value.hex()}\n' for name, value in (keys.items())))
    for kind in ('base', 'patch'):
        nsp = build_pfs0([('%016x.nca' % int.from_bytes(hashlib.sha256(nca := make_game_nca(rng, kind)).digest()[:8], 'big'), nca)])
        (out / ('base.nsp' if kind == 'base' else 'update.nsp')).write_bytes(nsp)
    print(f'wrote {out}/prod.keys base.nsp update.nsp (seed {seed:#x})')

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'work/synth'
    seed = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x5EED5EED
    write_all(out, seed)

if __name__ == '__main__':
    import sys
    main()
