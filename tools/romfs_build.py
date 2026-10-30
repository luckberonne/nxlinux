#!/usr/bin/env python3
"""Builds a standard Horizon RomFS image (the level-5 data a title reads through fsp-srv's OpenDataStorageByCurrentProcess) from a directory tree.

  romfs_build.py DIR OUT.romfs

Layout, as LibHac's RomFsBuilder and Atmosphère's romfs code write it: a 0x50-byte header of ten u64 (header size, then offset and size of the directory hash table, directory
entries, file hash table, file entries, then the file data offset), the four tables, and the file data with each file on a 0x10 boundary. An entry's name is padded to 4 bytes; a
name lookup hashes (parent id, name) into a table whose length is the odd, small-prime-free count the format asks for. Entries are sorted by name within a directory."""
import os, struct, sys

NONE = 0xFFFFFFFF


def hash_count(n):
    if n < 3: return 3
    if n < 19: return n | 1
    while any(n % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)): n += 1
    return n


def name_hash(parent, name):
    h = (123456789 ^ parent) & 0xFFFFFFFF
    for c in name: h = (((h >> 5) | (h << 27)) & 0xFFFFFFFF) ^ c
    return h


def build(src, out):
    dirs = [{'path': src, 'name': b'', 'parent': 0, 'sib': NONE, 'child': NONE, 'file': NONE, 'hash': NONE}]
    files = []
    def walk(di):
        d = dirs[di]
        names = sorted(os.listdir(d['path']), key=lambda s: s.encode())
        subs = [n for n in names if os.path.isdir(os.path.join(d['path'], n))]
        regs = [n for n in names if not os.path.isdir(os.path.join(d['path'], n))]
        last = None
        for n in subs:                                          # directory entries get their ids (byte offsets) once the sizes are known; link by index for now
            dirs.append({'path': os.path.join(d['path'], n), 'name': n.encode(), 'parent': di, 'sib': NONE, 'child': NONE, 'file': NONE, 'hash': NONE})
            k = len(dirs) - 1
            if last is None: d['child'] = k
            else: dirs[last]['sib'] = k
            last = k
        last = None
        for n in regs:
            files.append({'path': os.path.join(d['path'], n), 'name': n.encode(), 'parent': di, 'sib': NONE, 'hash': NONE, 'size': os.path.getsize(os.path.join(d['path'], n))})
            k = len(files) - 1
            if last is None: d['file'] = k
            else: files[last]['sib'] = k
            last = k
        for k in [i for i, e in enumerate(dirs) if e['parent'] == di and i != di and 'walked' not in e]:
            dirs[k]['walked'] = 1; walk(k)
    dirs[0]['walked'] = 1; walk(0)
    # byte offsets of the entries inside their tables
    off = 0
    for e in dirs: e['off'] = off; off += 24 + ((len(e['name']) + 3) & ~3)
    dir_meta_size = off
    off = 0
    for e in files: e['off'] = off; off += 32 + ((len(e['name']) + 3) & ~3)
    file_meta_size = off
    pos = 0
    for e in files: e['data'] = pos; pos = (pos + e['size'] + 15) & ~15
    dh, fh = [NONE] * hash_count(len(dirs)), [NONE] * hash_count(len(files))
    for e in dirs:
        par = dirs[e['parent']]['off'] if e is not dirs[0] else 0
        i = name_hash(par, e['name']) % len(dh); e['hash'] = dh[i]; dh[i] = e['off']
    for e in files:
        i = name_hash(dirs[e['parent']]['off'], e['name']) % len(fh); e['hash'] = fh[i]; fh[i] = e['off']
    def a(x, al): return (x + al - 1) & ~(al - 1)
    dir_hash_off = 0x200
    dir_meta_off = a(dir_hash_off + 4 * len(dh), 4)
    file_hash_off = a(dir_meta_off + dir_meta_size, 4)
    file_meta_off = a(file_hash_off + 4 * len(fh), 4)
    data_off = a(file_meta_off + file_meta_size, 0x200)
    dmeta = bytearray()
    for e in dirs:
        par = dirs[e['parent']]['off']
        sib = dirs[e['sib']]['off'] if e['sib'] != NONE else NONE
        ch = dirs[e['child']]['off'] if e['child'] != NONE else NONE
        fl = files[e['file']]['off'] if e['file'] != NONE else NONE
        dmeta += struct.pack('<IIIIII', par, sib, ch, fl, e['hash'], len(e['name'])) + e['name'].ljust((len(e['name']) + 3) & ~3, b'\0')
    fmeta = bytearray()
    for e in files:
        sib = files[e['sib']]['off'] if e['sib'] != NONE else NONE
        fmeta += struct.pack('<IIQQII', dirs[e['parent']]['off'], sib, e['data'], e['size'], e['hash'], len(e['name'])) + e['name'].ljust((len(e['name']) + 3) & ~3, b'\0')
    with open(out, 'wb') as f:
        f.write(struct.pack('<10Q', 0x50, dir_hash_off, 4 * len(dh), dir_meta_off, len(dmeta), file_hash_off, 4 * len(fh), file_meta_off, len(fmeta), data_off))
        f.seek(dir_hash_off); f.write(struct.pack('<%dI' % len(dh), *dh))
        f.seek(dir_meta_off); f.write(dmeta)
        f.seek(file_hash_off); f.write(struct.pack('<%dI' % len(fh), *fh))
        f.seek(file_meta_off); f.write(fmeta)
        for e in files:
            f.seek(data_off + e['data'])
            with open(e['path'], 'rb') as g:
                while True:
                    b = g.read(8 << 20)
                    if not b: break
                    f.write(b)
        f.truncate(data_off + pos)
    return len(dirs), len(files)


if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    nd, nf = build(sys.argv[1], sys.argv[2])
    print(f'{sys.argv[2]}: {nd} directories, {nf} files')
