#!/usr/bin/env python3
"""tools/nup_extract.py: build small NUP containers (base, update, raw and zstd files) with a writer that follows the v1 layout, extract them and compare the trees. Also checks the TOC hash, path-name safety, and that an update overlays the base."""
import hashlib, os, struct, sys, tempfile
from pathlib import Path
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import nup_extract as ne

try:
    from compression.zstd import compress as zcompress
except ImportError:
    zcompress = None


def build(path, packages, zstd=True):
    """packages: [{'kind', 'title', 'version', 'exefs': tree|None, 'romfs': tree|None}]; a tree is {name: bytes | {subtree}}."""
    dirs, files = [], []                 # dirs: [name, [node ids]] (node: ('d', i) or ('f', i)); files: {'name', 'data'}
    def add_dir(name, tree):
        idx = len(dirs)
        dirs.append([name, []])
        for k, v in tree.items():
            if isinstance(v, dict):
                dirs[idx][1].append(('d', add_dir(k, v)))
            else:
                files.append({'name': k, 'data': v})
                dirs[idx][1].append(('f', len(files) - 1))
        return idx
    pkgs = []
    for p in packages:
        root = add_dir('', {})
        exefs = add_dir('ExeFS', p['exefs']) if p.get('exefs') is not None else ne.NO_DIR
        romfs = add_dir('RomFS', p['romfs']) if p.get('romfs') is not None else ne.NO_DIR
        pkgs.append((p, root, exefs, romfs))
    dir_count = len(dirs)
    # blob: each file's extent, zstd when it helps
    extents = []
    for f in files:
        data = f['data']
        if zstd and zcompress and data and len(zcompress(data)) < len(data):
            payload, codec = zcompress(data), ne.CODEC_ZSTD
        else:
            payload, codec = data, ne.CODEC_RAW
        extents.append((len(payload), codec, payload))
    # string pool: every name once, in a fixed order
    pool = bytearray()
    def name(s):
        off = len(pool); pool.extend(s.encode()); return off, len(s.encode())
    dir_names = [name(nm) for nm, _ in dirs]
    file_names = [name(f['name']) for f in files]
    children, dir_recs = [], []
    for (off, n), (_, nodes) in zip(dir_names, dirs):
        first = len(children)
        for kind, i in nodes:
            children.append(i if kind == 'd' else dir_count + i)
        dir_recs.append(struct.pack('<IIII', off, n, first, len(nodes)) + bytes(8))
    pkg_recs = [struct.pack('<QIIIIBB', p['title'], root, exefs, romfs, p['version'], p['kind'], 0).ljust(0x40, b'\0')
                for p, root, exefs, romfs in pkgs]

    def toc_bytes(offsets):
        file_recs = []
        for f, (esize, codec, _), eoff, (off, n) in zip(files, extents, offsets, file_names):
            sha = hashlib.sha256(f['data']).digest()
            file_recs.append(struct.pack('<IIQQQ', off, n, len(f['data']), eoff, esize) + sha + bytes([codec, 0]) + bytes(14))
        body = b''.join(pkg_recs) + b''.join(dir_recs) + b''.join(file_recs) + struct.pack(f'<{len(children)}I', *children)
        pool_off = 0x30 + len(body)
        hdr = b'TOC\x01' + struct.pack('<IQQQQ', len(pkgs), dir_count, len(files), pool_off, len(pool)) + bytes(8)
        return hdr + body + bytes(pool)

    # the TOC's size does not depend on the extent offsets, so lay out the blob after a first encoding
    toc_size = len(toc_bytes([0] * len(files)))
    blob_off = (0x80 + toc_size + 15) & ~15
    offsets, pos = [], blob_off
    for esize, _, _ in extents:
        pos = (pos + 15) & ~15
        offsets.append(pos if esize else 0)
        pos += esize
    toc = toc_bytes(offsets)
    assert len(toc) == toc_size
    out = bytearray(0x80) + toc
    out += bytes(blob_off - len(out))
    for (esize, codec, payload), eoff in zip(extents, offsets):
        if esize:
            out += bytes(eoff - len(out)) + payload
    file_size = len(out)
    sb = bytearray(0x80)
    sb[0:4] = b'NUP\x01'
    struct.pack_into('<HH', sb, 4, 0x80, 1)
    struct.pack_into('<QQ', sb, 8, 0x80, toc_size)
    sb[0x18:0x38] = hashlib.sha256(toc).digest()
    struct.pack_into('<QQII', sb, 0x38, blob_off, file_size, len(pkgs), 0)
    out[0:0x80] = sb
    Path(path).write_bytes(bytes(out))


def tree_of(root):
    """{relpath: bytes} for every file under root, with '/' separators."""
    found = {}
    for p in Path(root).rglob('*'):
        if p.is_file():
            found[p.relative_to(root).as_posix()] = p.read_bytes()
    return found


def flat(tree, prefix=''):
    out = {}
    for k, v in tree.items():
        if isinstance(v, dict):
            out.update(flat(v, prefix + k + '/'))
        else:
            out[prefix + k] = v
    return out


BIG = bytes(range(256)) * 12000          # ~3 MB, compressible: exercises the zstd path
BASE_EXEFS = {'main': b'NSO0' + bytes(60), 'main.npdm': b'META' + bytes(124), 'rtld': b'rtld-code'}
BASE_ROMFS = {'big.bin': BIG, 'dir': {'sub': {'b.txt': b'hello'}}, 'empty': b'', 'x.txt': b'base-x', 'y.txt': b'base-y'}


def test_roundtrip(tmp):
    src = tmp / 'game.nup'
    build(src, [{'kind': 0, 'title': 0x0400486024DEC000, 'version': 0x010000, 'exefs': BASE_EXEFS, 'romfs': BASE_ROMFS}])
    out = tmp / 'out'
    ne.extract(src, out, log=lambda *_: None)
    assert tree_of(out / 'ExeFS') == flat(BASE_EXEFS), 'ExeFS differs'
    assert tree_of(out / 'RomFS') == flat(BASE_ROMFS), 'RomFS differs'


def test_update_overlays(tmp):
    src = tmp / 'game.nup'
    upd_exefs = {'main': b'NSO0-new' + bytes(56)}
    upd_romfs = {'x.txt': b'update-x', 'dir': {'z.txt': b'added'}}
    build(src, [
        {'kind': 1, 'title': 0x0400486024DEC800, 'version': 0x020000, 'exefs': upd_exefs, 'romfs': upd_romfs},  # listed first on purpose
        {'kind': 0, 'title': 0x0400486024DEC000, 'version': 0x010000, 'exefs': BASE_EXEFS, 'romfs': BASE_ROMFS},
    ])
    out = tmp / 'out'
    ne.extract(src, out, log=lambda *_: None)
    assert tree_of(out / 'ExeFS') == flat(upd_exefs), 'update ExeFS must replace the base ExeFS'
    want = flat(BASE_ROMFS)
    want.update({'x.txt': b'update-x', 'dir/z.txt': b'added'})
    assert tree_of(out / 'RomFS') == want, 'update RomFS must overlay the base RomFS'


def test_bad_toc_hash(tmp):
    src = tmp / 'game.nup'
    build(src, [{'kind': 0, 'title': 1, 'version': 0, 'exefs': {'main': b'x'}, 'romfs': None}])
    raw = bytearray(src.read_bytes())
    raw[0x80 + 0x30 + 1] ^= 0xFF        # corrupt a byte inside the package records of the TOC
    src.write_bytes(bytes(raw))
    try:
        ne.extract(src, tmp / 'out', log=lambda *_: None)
    except ne.NupError as e:
        assert 'TOC' in str(e), e
        return
    raise AssertionError('a corrupted TOC was accepted')


def test_unsafe_name(tmp):
    src = tmp / 'game.nup'
    build(src, [{'kind': 0, 'title': 1, 'version': 0, 'exefs': None, 'romfs': {'..': {'evil': b'x'}}}])
    try:
        ne.extract(src, tmp / 'out', log=lambda *_: None)
    except ne.NupError as e:
        assert 'unsafe name' in str(e), e
        assert not (tmp / 'evil').exists()
        return
    raise AssertionError('a parent-directory name was accepted')


def main():
    tests = [test_roundtrip, test_update_overlays, test_bad_toc_hash, test_unsafe_name]
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            sub = Path(d) / t.__name__
            sub.mkdir()
            t(sub)
            print('ok', t.__name__)
    print('all nup_extract tests passed' + ('' if zcompress else ' (zstd writer unavailable: raw files only)'))


if __name__ == '__main__':
    main()
