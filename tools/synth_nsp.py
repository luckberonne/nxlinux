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
  · Code section: a PFS0 with the SHA-256 hash tree: the section's FS header
    carries a master hash over the SHA-256 table (one digest per 0x10000 block
    of the file system); the table directly precedes the file system data
    (InitIvfcForPartitionFs).
  · Data section: an IVFC-wrapped RomFS (HashType.Ivfc in the FS header). The
    ivfc struct read by InitIvfcForRomFs names three regions: region 0 holds
    the SHA-256 table over region 1's 0x1000 blocks, region 1 the table over
    the RomFS data's 0x4000 blocks, and the master hash (in the fs header) the
    table over region 0 as a single 0x1000 block. Every digest is plain
    SHA-256 over the zero-padded block: Tools/FsSystem's
    IntegrityVerificationStorage pads each touched sector for its digest and,
    for RomFs/PartitionFs, hashes without salt and without the validation bit
    (that is save-data only). Inside, the RomFS is what RomFsFileSystem reads:
    a 0x50 header of nine offsets, int hash buckets (RomEntryKey.GetRomHashCode
    % bucket count), 24-byte directory and 32-byte file entries laid out as the
    reader's RomFsEntry, with the sibling chains the enumeration walks and the
    bucket chains the lookup walks.
  · No ticket: the key area holds plain content keys, not title-key ones.
  · prod.keys: only header_key and key_area_key_application_00. Deliberately no
    master_key_00 and no *_source keys: LibHac ships the real source constants
    (DefaultKeySet) and re-derives the finals from them (KeyDerivation), so a
    loaded master_key_00 would clobber our finals. With the master keys zeroed
    the derivation steps skip and the two throwaway keys survive.

Writes work/synth/{prod.keys, base.nsp, update.nsp} by default (gitignored); every
byte is pseudorandom from a fixed seed, so a CI test can pin the artifacts.
"""
import argparse
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
FS_FORMAT_ROMFS = 0
FS_FORMAT_PFS0 = 1
FS_HASH_IVFC = 3
FS_HASH_SHA256 = 2
FS_ENCRYPT_CTR = 3
GF_REDUCTION = 0x87       # x^128 + x^7 + x^2 + x + 1, the tweak's wrap term
CTR_SLOT = 2              # NcaKeyType.AesCtr
DEFAULT_SEED = 0x5EED5EED

IVFC_MAGIC = b'IVFC'
IVFC_VERSION = 0x10000
IVFC_LEVELS = 4           # master hash + three regions; region 2 is the RomFS data
ROMFS_BSP = (12, 12, 14)  # log2 block size hashed for region 0 / region 1 / the data
ROMFS_DATA_UNIT = 1 << ROMFS_BSP[2]
ROMFS_TABLE_LIMIT = 0x1000  # region 0 is covered as one master-hash block (no room for more)
ROMFS_DIR_ENTRY = 24      # Parent, NextSibling, ChildDir, ChildFile, Next, KeyLength
ROMFS_FILE_ENTRY = 32     # Parent, NextSibling, Offset, Length, Next, KeyLength

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

def build_nso(text, ro, data, bss, build_id, mos=None):
    """NSO0 with the flags clear: no compression, no segment hashes. load() requires
    text at 0 in memory, ro and data after it on whole pages, nothing overlapping
    (a gap is allowed); the file's segments follow right after the header. mos, when
    given, places the segments at those memory offsets instead of packing them tight
    (what packing an ELF keeps, so the guest's prelinked addresses still land)."""
    assert len(build_id) == 0x20
    tight = (0, _page_up(len(text)), _page_up(len(text)) + _page_up(len(ro)))
    if mos is None:
        mos = tight
    else:
        mos = list(mos)
        while len(mos) < 3:                     # the ELF can fold empty segments away
            mos.append(tight[len(mos)])
    assert mos[0] == 0 and mos[1] >= len(text) and mos[2] >= mos[1] + len(ro)   # load() checks these
    assert all(m % PAGE == 0 for m in mos)                                      # load() checks these too
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

# --- RomFS (the NCA Data section's file system) -----------------------------------------

def _romfs_name_hash(parent, name):
    """RomEntryKey.GetRomHashCode: a rotate-27 byte mix over the name."""
    h = (0x75BCD15 ^ (parent & 0xFFFFFFFF)) & 0xFFFFFFFF
    for b in name:
        h = (b ^ ((h << 27) | (h >> 5))) & 0xFFFFFFFF
    return h

def _prime_at_least(n):
    n = max(3, n)
    while any(n % d == 0 for d in range(2, min(n, 47))):
        n += 1
    return n

def _sibling_chain(items, parent_of):
    """(next-sibling map, {parent: first child offset}) for entries: whoever shares
    a parent is consecutive in items. These are the chains FindNextFile and
    FindNextDirectory walk (the bucket chains are what the lookups walk)."""
    sib, first = {}, {}
    prev_key, prev_parent = None, object()
    for key, off in items:
        parent = parent_of(key)
        if prev_key is not None and prev_parent == parent:
            sib[prev_key] = off
        else:
            first[parent] = off
        prev_key, prev_parent = key, parent
    return sib, first

def build_romfs(files):
    """files: {posix rel path: content} -> a RomFS image as RomFsFileSystem reads it.
    Layout: 0x50 header of nine offsets, then dir hash table, dir entries, file
    hash table, file entries, file data. The root is entry 0 of the dir table
    under key (parent 0, empty name); dir entries are 24 bytes and file entries
    32 (the reader's RomFsEntry), each followed by the name, no NUL, rounded to
    four bytes."""
    assert files
    tree = {'dirs': {}, 'files': []}
    for path, content in files.items():
        node = tree
        parts = path.split('/')
        for part in parts[:-1]:
            node = node['dirs'].setdefault(part, {'dirs': {}, 'files': []})
        node['files'].append((parts[-1], content))
    dirs = []                                   # (path tuple, node), parents first
    def collect(node, path):
        node['dirs'] = dict(sorted(node['dirs'].items()))
        node['files'].sort(key=lambda item: item[0])
        dirs.append((path, node))
        for name, child in node['dirs'].items():
            collect(child, path + (name,))
    collect(tree, ())

    dir_meta, file_meta, file_data = {}, {}, {}
    file_rows = []
    at = 0
    for path, node in dirs:
        dir_meta[path] = at
        at += _align(ROMFS_DIR_ENTRY + len(path[-1] if path else ''), 4)
        for fname, content in node['files']:
            file_rows.append((path, fname, content))
    at = 0
    for dpath, fname, content in file_rows:
        file_data[dpath + (fname,)], at = at, at + len(content)
    at = 0
    for dpath, fname, content in file_rows:
        file_meta[dpath + (fname,)], at = at, at + _align(ROMFS_FILE_ENTRY + len(fname), 4)

    # the root's own row is skipped: it has no parent and its sibling stays -1
    dir_sib, first_dir = _sibling_chain([(p, dir_meta[p]) for p, _ in dirs if p],
                                        lambda p: p[:-1])
    file_sib, first_file = _sibling_chain([(p + (n,), file_meta[p + (n,)]) for p, n, _ in file_rows],
                                          lambda p: p[:-1])

    d_buckets = [-1] * _prime_at_least(len(dirs))
    d_next = {}
    for path, _ in dirs:
        parent = dir_meta.get(path[:-1], 0) if path else 0
        b = _romfs_name_hash(parent, path[-1].encode() if path else b'') % len(d_buckets)
        d_next[path] = d_buckets[b]
        d_buckets[b] = dir_meta[path]
    f_buckets = [-1] * _prime_at_least(len(file_rows))
    f_next = {}
    for dpath, fname, content in file_rows:
        b = _romfs_name_hash(dir_meta[dpath], fname.encode()) % len(f_buckets)
        f_next[dpath + (fname,)] = f_buckets[b]
        f_buckets[b] = file_meta[dpath + (fname,)]

    dblob = bytearray()
    for path, node in dirs:
        name = b'' if not path else path[-1].encode()
        parent = 0 if not path else dir_meta[path[:-1]]
        sib = dir_sib.get(path, -1) if path else -1
        dblob += struct.pack('<IiiiiI', parent, sib, first_dir.get(path, -1),
                             first_file.get(path, -1), d_next[path], len(name))
        dblob += name
        used = ROMFS_DIR_ENTRY + len(name)
        dblob += b'\0' * (_align(used, 4) - used)
    fblob = bytearray()
    for dpath, fname, content in file_rows:
        key = dpath + (fname,)
        name = fname.encode()
        fblob += struct.pack('<IiQQi' + 'I', dir_meta[dpath], file_sib.get(key, -1),
                             file_data[key], len(content), f_next[key], len(name))
        fblob += name
        used = ROMFS_FILE_ENTRY + len(name)
        fblob += b'\0' * (_align(used, 4) - used)

    dh_off = 0x50
    dm_off = _align(dh_off + len(d_buckets) * 4, 4)
    fh_off = _align(dm_off + len(dblob), 4)
    fm_off = _align(fh_off + len(f_buckets) * 4, 4)
    data_off = _align(fm_off + len(fblob), 16)
    # RomfsHeader reads the nine offsets as u64s from 0x08 (the u32 at 0x04 is unused)
    header = struct.pack('<II', 0x50, 0) + struct.pack(
        '<9Q', dh_off, len(d_buckets) * 4, dm_off, len(dblob),
        fh_off, len(f_buckets) * 4, fm_off, len(fblob), data_off)
    image = bytearray(header)
    image += b'\0' * (dh_off - len(image))      # the header block is 0x50 bytes
    assert len(image) == dh_off
    image += struct.pack('<%di' % len(d_buckets), *d_buckets)
    assert len(image) == dm_off
    image += dblob
    assert len(image) == fh_off
    image += struct.pack('%di' % len(f_buckets), *f_buckets)
    assert len(image) == fm_off
    image += fblob
    assert len(image) <= data_off
    image += b'\0' * (data_off - len(image))    # data starts at its own aligned offset
    assert len(image) == data_off
    pos = data_off
    for dpath, fname, content in file_rows:
        assert pos == data_off + file_data[dpath + (fname,)]
        image += content
        pos += len(content)
    image += b'\0' * (_align(len(image), 16) - len(image))
    return bytes(image)

def _block_hashes(data, unit):
    """SHA-256 per unit-sized block, the last zero-padded. The reader computes each
    level's digest over the touched sector with the read's tail zeroed (RomFs type
    hashes the padded sector), so the stored digests must cover the padding."""
    out = bytearray()
    for i in range(0, len(data), unit):
        chunk = data[i:i + unit]
        out += hashlib.sha256(chunk + b'\0' * (unit - len(chunk))).digest()
    return bytes(out)

def build_fs_header_romfs(master, regions, image_len, counter=(1, 0)):
    """FS header of a Data section: RomFS / IVFC / CTR. The ivfc struct sits at
    +0x08: magic, version 0x10000, master hash size, level count 4; six level
    entries at +0x18 (three used: region 0 = the table over region 1, region 1
    = the table over the data, region 2 = the data), the salt source at +0xA8
    left zero (RomFs hashing takes no salt), the master hash at +0xC8, the CTR
    counter pair at +0x140."""
    fs = bytearray(0x200)
    struct.pack_into('<H', fs, 0, 2)                    # header version
    fs[2] = FS_FORMAT_ROMFS
    fs[3] = FS_HASH_IVFC
    fs[4] = FS_ENCRYPT_CTR
    struct.pack_into('<4siii', fs, 8, IVFC_MAGIC, IVFC_VERSION, len(master), IVFC_LEVELS)
    for j, (off, size, bsp) in enumerate(regions):
        struct.pack_into('<qqii', fs, 24 + j * 24, off, size, bsp, 0)
    fs[200:200 + len(master)] = master
    struct.pack_into('<ii', fs, 320, counter[0], counter[1])
    return fs

def build_romfs_section(image):
    """(fs header, section data) for the Data section: region 0, region 1, then the
    RomFS image. The master hash covers region 0 as one 0x1000 block, so region
    0 (one digest per 0x1000 of region 1) stays a single-entry table and the
    image stays under 2 MiB at the current block sizes."""
    assert len(image) and len(image) % 16 == 0
    assert len(image) <= (ROMFS_TABLE_LIMIT // 32) * ROMFS_DATA_UNIT
    r1 = _block_hashes(image, ROMFS_DATA_UNIT)
    r0 = _block_hashes(r1, 1 << ROMFS_BSP[1])
    master = _block_hashes(r0, 1 << ROMFS_BSP[0])
    assert len(r0) <= ROMFS_TABLE_LIMIT
    r1_off = _align(len(r0), 16)
    data_off = _align(r1_off + len(r1), 16)
    section = r0 + b'\0' * (r1_off - len(r0)) + r1 + b'\0' * (data_off - r1_off - len(r1)) + image
    fs = build_fs_header_romfs(master, [(0, len(r0), ROMFS_BSP[0]),
                                        (r1_off, len(r1), ROMFS_BSP[1]),
                                        (data_off, len(image), ROMFS_BSP[2])], len(image))
    return fs, section

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

# --- NSO from a compiled guest (the clang + lld output CMake builds) ---------------------

def _elf_notes(elf):
    """(name, type, desc) of every note the file carries, PT_NOTE by PT_NOTE (4-aligned)."""
    phoff = int.from_bytes(elf[0x20:0x28], 'little')
    phentsize = int.from_bytes(elf[0x36:0x38], 'little')
    phnum = int.from_bytes(elf[0x38:0x3a], 'little')
    out = []
    for i in range(phnum):
        p = elf[phoff + i * phentsize:phoff + (i + 1) * phentsize]
        if int.from_bytes(p[0:4], 'little') != 4:                       # PT_NOTE
            continue
        off = int.from_bytes(p[8:16], 'little')
        size = int.from_bytes(p[32:40], 'little')
        blob, at = elf[off:off + size], 0
        while at + 12 <= len(blob):
            namesz = int.from_bytes(blob[at:at + 4], 'little')
            descsz = int.from_bytes(blob[at + 4:at + 8], 'little')
            note_type = int.from_bytes(blob[at + 8:at + 12], 'little')
            at += 12
            name = bytes(blob[at:at + namesz]); at = (at + namesz + 3) & ~3
            desc = bytes(blob[at:at + descsz]); at = (at + descsz + 3) & ~3
            out.append((name, note_type, desc))
    return out

def build_nso_from_elf(elf):
    """A compiled guest ELF64 (what CMake's clang + lld builds for tests/guest) -> NSO0
    with plain segments, so the same program flows nx_extract -> nso.py -> module ->
    kernel. The rule is the loader's own, told back in the error: up to three PT_LOADs
    sorted by vaddr are text, ro, data, each vaddr on a 0x1000 page, text at 0, none
    overlapping (a page gap is allowed), and only the last one may carry bss. The
    segments keep their ELF vaddrs as memory offsets, so prelinked addresses still
    resolve. The build ID is the ELF's GNU note when it has one."""
    if elf[0:4] != b'\x7fELF' or elf[4] != 2 or elf[5] != 1:
        raise ValueError('the guest is not a little-endian ELF64')
    phoff = int.from_bytes(elf[0x20:0x28], 'little')
    phentsize = int.from_bytes(elf[0x36:0x38], 'little')
    phnum = int.from_bytes(elf[0x38:0x3a], 'little')
    loads = []
    for i in range(phnum):
        p = elf[phoff + i * phentsize:phoff + (i + 1) * phentsize]
        if int.from_bytes(p[0:4], 'little') != 1:                       # PT_LOAD
            continue
        vaddr = int.from_bytes(p[16:24], 'little')
        offset = int.from_bytes(p[8:16], 'little')
        filesz = int.from_bytes(p[32:40], 'little')
        memsz = int.from_bytes(p[40:48], 'little')
        loads.append((vaddr, offset, filesz, memsz))
    if not 0 < len(loads) <= 3:
        raise ValueError(f'{len(loads)} PT_LOADs: an NSO has at most text, ro and data '
                         '(link the guest flat: text, ro, data)')
    loads.sort()
    if loads[0][0] != 0 or loads[0][2] == 0:
        raise ValueError(f'the first PT_LOAD is at {loads[0][0]:#x}: text has to start at 0')
    for prev, cur in zip(loads, loads[1:]):
        if cur[0] % PAGE or cur[0] < _page_up(prev[0] + prev[2]):
            raise ValueError(f'a PT_LOAD at {cur[0]:#x} is not on a page after the one '
                             f'before (which ends at {prev[0] + prev[2]:#x}): '
                             'link the guest page-aligned flat (text, ro, data)')
    for vaddr, _, filesz, memsz in loads[:-1]:
        if memsz != filesz:                                             # only data carries bss
            raise ValueError(f'the PT_LOAD at {vaddr:#x} has p_memsz past its p_filesz; '
                             'an NSO has one bss size (the data segment\'s)')
    bss = loads[-1][3] - loads[-1][2]
    segs = [bytes(elf[off:off + filesz]) for _, off, filesz, _ in loads]
    while len(segs) < 3:
        segs.append(b'')
    for name, note_type, desc in _elf_notes(elf):
        if name == b'GNU\x00' and note_type == 3 and len(desc) <= 0x20: # NT_GNU_BUILD_ID
            return build_nso(*segs, bss, desc.ljust(0x20, b'\0'), mos=[l[0] for l in loads])
    return build_nso(*segs, bss, hashlib.sha256(elf).digest(), mos=[l[0] for l in loads])

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

def make_nacp():
    """A Control NACP (0x4000, the modern size) with the fake game's identity:
    entry 0's language, name and publisher, plus the display version at 0x3000."""
    nacp = bytearray(0x4000)
    struct.pack_into('<I', nacp, 0, 1)                    # language 1 = English US
    nacp[4:4 + 0x200] = b'Synthetic Test\0'.ljust(0x200, b'\0')
    nacp[0x204:0x204 + 0x100] = b'synth-nsp\0'.ljust(0x100, b'\0')
    nacp[0x3000:0x3010] = b'1.0.0\0'.ljust(0x10, b'\0')
    return bytes(nacp)

def make_romfs(variant):
    """The fake game's RomFS: two text files and a pseudorandom blob spanning the
    0x4000 data-block boundary, so reads touch more than one integrity block,
    plus a control.nacp with the title's metadata. The update ships a changed
    config and a file the base does not have."""
    files = {
        'data/config.txt': b'synthetic romfs config\nvariant: %s\n' % variant.encode(),
        'assets/greeting.txt': b'hello from the synthetic RomFS (%s)\n' % variant.encode(),
        'assets/tile.dat': rng_rand(0x7171, 0x4800),
        'control.nacp': make_nacp(),
    }
    if variant == 'update':
        files['data/extra.txt'] = b'only in the update\n'
    return build_romfs(files)

def make_game_nca(header_key, kaek, module, romfs=None):
    """The program NCA: the code section's PFS0 with the module, and (by default)
    the Data section with the variant's RomFS."""
    pfs = build_pfs0([('main', module)])
    assert len(pfs) % 16 == 0
    sections = [build_pfs_section(pfs)]
    if romfs is not None:
        sections.append(build_romfs_section(romfs))
    return build_nca3(sections, TITLE_ID, CONTENT_TYPE_PROGRAM, header_key, kaek)

def synth_sequence(seed):
    """The exact byte stream write_all generates, so tests can expect outputs:
    (header_key, kaek, base module, update module) for one seed."""
    rng = random.Random(seed)
    header_key = rng.randbytes(32)
    kaek = rng.randbytes(16)
    return header_key, kaek, _module(rng, 'base'), _module(rng, 'patch')

def write_all(out, seed=DEFAULT_SEED, with_romfs=True, main_nso=None):
    """Make prod.keys, base.nsp and update.nsp. main_nso, when given, replaces the
    base title's own module (an NSO packed from a real compiled guest, so the
    pipeline nx_extract -> nso.py -> module -> kernel runs on actual aarch64 code);
    the update keeps the synthetic patch module so the merge path still works."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    header_key, kaek, base_module, update_module = synth_sequence(seed)
    if main_nso is not None:
        base_module = main_nso
    (out / 'prod.keys').write_text(
        '# synthetic keyset for the pipeline tests (docs/plan.md); not from any console\n'
        f'header_key = {header_key.hex()}\n'
        f'key_area_key_application_00 = {kaek.hex()}\n')
    romfs = {'base.nsp': make_romfs('base'), 'update.nsp': make_romfs('update')} if with_romfs else {}
    for name, module in (('base.nsp', base_module), ('update.nsp', update_module)):
        nca = make_game_nca(header_key, kaek, module, romfs.get(name))
        (out / name).write_bytes(build_pfs0([(content_id(nca) + '.nca', nca)]))
    print(f'wrote {out}/prod.keys base.nsp update.nsp (seed {seed:#x})')

def content_id(nca):
    """The .nca file name inside an NSP: hex of what would be the content's first id."""
    return hashlib.sha256(nca).hexdigest()[:32]

def main():
    ap = argparse.ArgumentParser(description='write a synthetic title: prod.keys, base.nsp, update.nsp')
    ap.add_argument('out', nargs='?', default='work/synth')
    ap.add_argument('--seed', type=lambda x: int(x, 0), default=DEFAULT_SEED)
    ap.add_argument('--no-romfs', action='store_true')
    ap.add_argument('--main-elf', metavar='PATH',
                    help='use this compiled guest (ELF from clang + lld, or already an NSO) '
                         'as the base title\'s ExeFS main')
    args = ap.parse_args()
    main_nso = None
    if args.main_elf:
        elf = Path(args.main_elf).read_bytes()
        main_nso = elf if elf[0:4] == b'NSO0' else build_nso_from_elf(elf)
    write_all(args.out, args.seed, not args.no_romfs, main_nso)

if __name__ == '__main__':
    sys.exit(main())
