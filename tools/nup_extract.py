#!/usr/bin/env python3
"""Extract the ExeFS and RomFS of a NUP (.nup) title container. The contents are plain, so no keys are needed.

  nup_extract.py FILE.nup OUT [--exefs-only] [--no-verify]
  nup_extract.py FILE.nup --info

Writes OUT/ExeFS and OUT/RomFS, the layout tools/nx_extract writes, so tools/run_title.py can use them. Packages are applied base first, then
each update over it: an update's ExeFS replaces the base's, and its RomFS files overwrite the base's at the same path. Add-on packages are
skipped. The format is the NUP v1 draft in github.com/a55uka/nup (SPEC.md); the TOC hash and each file's SHA-256 are checked."""
import argparse, hashlib, os, shutil, struct, sys
from pathlib import Path

NO_DIR = 0xFFFFFFFF
KIND_BASE, KIND_UPDATE, KIND_ADDON = 0, 1, 2
CODEC_RAW, CODEC_ZSTD = 0, 1
CHUNK = 1 << 20


class NupError(Exception):
    pass


def _zstd_decompressor():
    try:
        from compression.zstd import ZstdDecompressor          # Python 3.14
        return ZstdDecompressor()
    except ImportError:
        pass
    try:
        import zstandard
    except ImportError:
        raise NupError('zstd files need Python 3.14 (compression.zstd) or the zstandard package')
    return zstandard.ZstdDecompressor().decompressobj()


def _safe_name(name):
    if name in ('', '.', '..') or '/' in name or '\\' in name or '\0' in name:
        raise NupError(f'unsafe name in container: {name!r}')
    return name


class Nup:
    def __init__(self, path, verify=True):
        self.verify = verify
        self.f = open(path, 'rb')
        size = os.fstat(self.f.fileno()).st_size
        sb = self.f.read(0x80)
        if len(sb) < 0x80 or sb[:3] != b'NUP' or sb[3] != 1:
            raise NupError('not a NUP v1 container')
        hsize, hver = struct.unpack_from('<HH', sb, 4)
        toc_off, toc_size = struct.unpack_from('<QQ', sb, 8)
        toc_sha = sb[0x18:0x38]
        blob_off, file_size, pkg_count, flags = struct.unpack_from('<QQII', sb, 0x38)
        if hsize < 0x80 or hver != 1 or flags != 0:
            raise NupError(f'unsupported header (size {hsize:#x}, version {hver}, flags {flags:#x})')
        if toc_off < hsize or toc_off + toc_size > size or blob_off < toc_off + toc_size:
            raise NupError('inconsistent superblock offsets')
        if file_size != size:
            raise NupError(f'file_size {file_size} does not match the file ({size} bytes)')
        self.file_size = size
        self.f.seek(toc_off)
        toc = self.f.read(toc_size)
        if len(toc) != toc_size or toc[:4] != b'TOC\x01':
            raise NupError('TOC missing or wrong magic')
        if self.verify and hashlib.sha256(toc).digest() != toc_sha:
            raise NupError('TOC SHA-256 does not match the superblock')
        self.toc = toc
        n_pkg, self.dir_count, self.file_count, pool_off, pool_size = struct.unpack_from('<IQQQQ', toc, 4)
        if n_pkg != pkg_count or n_pkg < 1:
            raise NupError('package count disagrees between superblock and TOC')
        self.pool = toc[pool_off:pool_off + pool_size]
        if len(self.pool) != pool_size:
            raise NupError('string pool outside the TOC')
        self.pkg_base = 0x30
        self.dir_base = self.pkg_base + n_pkg * 0x40
        self.file_base = self.dir_base + self.dir_count * 0x18
        child_base = self.file_base + self.file_count * 0x50
        if child_base > pool_off or (pool_off - child_base) % 4:
            raise NupError('child table does not fit before the string pool')
        self.children = struct.unpack_from(f'<{(pool_off - child_base) // 4}I', toc, child_base)
        self.packages = []
        for i in range(n_pkg):
            o = self.pkg_base + i * 0x40
            tid, root, exefs, romfs, ver = struct.unpack_from('<QIIII', toc, o)
            self.packages.append({'title_id': tid, 'root': root, 'exefs': exefs, 'romfs': romfs,
                                  'version': ver, 'kind': toc[o + 0x18], 'flags': toc[o + 0x19]})

    def _name(self, off, n):
        if off + n > len(self.pool):
            raise NupError('name outside the string pool')
        return self.pool[off:off + n].decode('utf-8')

    def _dir(self, idx):
        if idx >= self.dir_count:
            raise NupError(f'directory index {idx} out of range')
        first, count = struct.unpack_from('<II', self.toc, self.dir_base + idx * 0x18 + 8)
        if first + count > len(self.children):
            raise NupError('children outside the child table')
        return first, count

    def _file(self, idx):
        if idx >= self.file_count:
            raise NupError(f'file index {idx} out of range')
        o = self.file_base + idx * 0x50
        name_off, name_len = struct.unpack_from('<II', self.toc, o)
        size, eoff, esize = struct.unpack_from('<QQQ', self.toc, o + 8)
        sha, codec, cflags = self.toc[o + 0x20:o + 0x40], self.toc[o + 0x40], self.toc[o + 0x41]
        if eoff + esize > self.file_size:
            raise NupError('extent outside the file')
        if codec == CODEC_RAW and esize != size:
            raise NupError('raw extent size differs from the file size')
        if codec not in (CODEC_RAW, CODEC_ZSTD) or cflags:
            raise NupError(f'unsupported codec {codec} / flags {cflags}')
        return {'name': self._name(name_off, name_len), 'size': size, 'off': eoff, 'esize': esize,
                'sha': sha, 'codec': codec}

    def walk(self, root):
        """Yield ('d', relpath) for each directory below root and ('f', relpath, file record) for each file."""
        seen = set()
        stack = [(root, '')]
        while stack:
            d, rel = stack.pop()
            if d in seen:
                raise NupError('directory linked twice')
            seen.add(d)
            first, count = self._dir(d)
            for node in self.children[first:first + count]:
                if node < self.dir_count:
                    name_off, name_len = struct.unpack_from('<II', self.toc, self.dir_base + node * 0x18)
                    p = (rel + _safe_name(self._name(name_off, name_len)) + '/')
                    yield ('d', p.rstrip('/'))
                    stack.append((node, p))
                else:
                    rec = self._file(node - self.dir_count)
                    yield ('f', rel + _safe_name(rec['name']), rec)

    def write_file(self, rec, dest):
        dest.parent.mkdir(parents=True, exist_ok=True)
        h = hashlib.sha256()
        with open(dest, 'wb') as out:
            self.f.seek(rec['off'])
            if rec['codec'] == CODEC_RAW:
                left = rec['esize']
                while left:
                    buf = self.f.read(min(CHUNK, left))
                    if not buf:
                        raise NupError('file ends inside an extent')
                    left -= len(buf)
                    h.update(buf)
                    out.write(buf)
            else:
                d = _zstd_decompressor()
                left = rec['esize']
                written = 0
                while not getattr(d, 'eof', False):
                    if left == 0:
                        raise NupError(f'zstd stream of {rec["name"]} ends early')
                    buf = self.f.read(min(CHUNK, left))
                    if not buf:
                        raise NupError('file ends inside an extent')
                    left -= len(buf)
                    part = d.decompress(buf)
                    h.update(part)
                    out.write(part)
                    written += len(part)
                if written != rec['size']:
                    raise NupError(f'{rec["name"]}: {written} bytes decoded, {rec["size"]} expected')
        if self.verify and h.digest() != rec['sha']:
            raise NupError(f'SHA-256 mismatch: {rec["name"]}')

    def extract_tree(self, root, dest):
        files = dirs = nbytes = 0
        dest.mkdir(parents=True, exist_ok=True)
        for item in self.walk(root):
            target = dest.joinpath(*item[1].split('/'))
            if item[0] == 'd':
                target.mkdir(parents=True, exist_ok=True)
                dirs += 1
            else:
                self.write_file(item[2], target)
                files += 1
                nbytes += item[2]['size']
        return files, dirs, nbytes


def extract(path, out, exefs_only=False, verify=True, log=print):
    out = Path(out)
    nup = Nup(path, verify)
    log(f'{Path(path).name}: {len(nup.packages)} packages, {nup.file_count} files, {nup.dir_count} directories')
    # the base package first, then the updates in container order
    order = sorted((p for p in nup.packages if p['kind'] in (KIND_BASE, KIND_UPDATE)), key=lambda p: p['kind'])
    for p in nup.packages:
        if p['kind'] not in (KIND_BASE, KIND_UPDATE):
            log(f'skipped package kind {p["kind"]} title {p["title_id"]:016x} (add-ons are not extracted)')
    for p in order:
        kind = 'base' if p['kind'] == KIND_BASE else 'update'
        log(f'{kind} {p["title_id"]:016x} v{p["version"] >> 16}.{(p["version"] >> 8) & 0xff}.{p["version"] & 0xff}')
        if p['exefs'] != NO_DIR:
            if (out / 'ExeFS').exists():
                shutil.rmtree(out / 'ExeFS')            # an update's ExeFS replaces the base's
            n, _, nb = nup.extract_tree(p['exefs'], out / 'ExeFS')
            log(f'  ExeFS: {n} files, {nb:#x} bytes')
        if not exefs_only and p['romfs'] != NO_DIR:
            n, d, nb = nup.extract_tree(p['romfs'], out / 'RomFS')   # overlays: an update's files win
            log(f'  RomFS: {n} files in {d} directories, {nb:#x} bytes')
    return out


def info(path, log=print):
    nup = Nup(path)
    log(f'NUP v1, {nup.file_size} bytes, {len(nup.packages)} packages, {nup.dir_count} directories, {nup.file_count} files')
    for p in nup.packages:
        kind = {KIND_BASE: 'base', KIND_UPDATE: 'update', KIND_ADDON: 'addon'}.get(p['kind'], f'kind {p["kind"]}')
        v = p['version']
        log(f'  {kind} title {p["title_id"]:016x} version {v >> 16}.{(v >> 8) & 0xff}.{v & 0xff} '
            f'ExeFS {"yes" if p["exefs"] != NO_DIR else "no"} RomFS {"yes" if p["romfs"] != NO_DIR else "no"}')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('nup')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--exefs-only', action='store_true', help='extract only the ExeFS')
    ap.add_argument('--no-verify', action='store_true', help='skip the TOC and per-file SHA-256 checks')
    ap.add_argument('--info', action='store_true', help='list the packages and exit')
    a = ap.parse_args(argv)
    try:
        if a.info:
            info(a.nup)
            return 0
        if not a.out:
            ap.error('OUT is required unless --info is given')
        extract(a.nup, a.out, a.exefs_only, not a.no_verify)
    except NupError as e:
        print(f'[RED] {e}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
