#!/usr/bin/env python3
"""tools/romfs_build.py: build a small tree, read the image back by walking the tables the way a RomFS reader does, and look names up through the hash tables."""
import os, struct, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import romfs_build as rb

def read(img):
    h = struct.unpack_from('<10Q', img, 0)
    dh_off, dh_sz, dm_off, dm_sz, fh_off, fh_sz, fm_off, fm_sz, data = h[1:]
    assert h[0] == 0x50
    def dent(o):
        par, sib, ch, fl, nxt, n = struct.unpack_from('<6I', img, dm_off + o)
        return par, sib, ch, fl, nxt, img[dm_off + o + 24:dm_off + o + 24 + n]
    def fent(o):
        par, sib, d, sz, nxt, n = struct.unpack_from('<IIQQII', img, fm_off + o)
        return par, sib, d, sz, nxt, img[fm_off + o + 32:fm_off + o + 32 + n]
    out = {}
    def walk(o, path):
        _, _, ch, fl, _, _ = dent(o)
        while fl != rb.NONE:
            _, sib, d, sz, _, nm = fent(fl)
            out[path + '/' + nm.decode()] = img[data + d:data + d + sz]; fl = sib
        while ch != rb.NONE:
            _, sib, _, _, _, nm = dent(ch)
            walk(ch, path + '/' + nm.decode()); ch = sib
    walk(0, '')
    # lookup through the file hash table
    def lookup(parent_off, name):
        n = fh_sz // 4
        o = struct.unpack_from('<I', img, fh_off + 4 * (rb.name_hash(parent_off, name) % n))[0]
        while o != rb.NONE:
            par, sib, d, sz, nxt, nm = fent(o)
            if par == parent_off and nm == name: return img[data + d:data + d + sz]
            o = nxt
    return out, lookup

with tempfile.TemporaryDirectory() as t:
    src = os.path.join(t, 'src'); os.makedirs(os.path.join(src, 'Course', 'Sub')); os.makedirs(os.path.join(src, 'Audio'))
    files = {'a.bin': b'x' * 5, 'Course/c1.szs': os.urandom(1000), 'Course/Sub/deep.txt': b'hello', 'Audio/empty': b'', 'Audio/z': b'\1' * 33}
    for k, v in files.items(): open(os.path.join(src, k), 'wb').write(v)
    for i in range(40): open(os.path.join(src, 'f%02d' % i), 'wb').write(bytes([i]) * (i + 1))   # enough files for a real hash table
    out = os.path.join(t, 'out.romfs')
    nd, nf = rb.build(src, out)
    assert (nd, nf) == (4, 45), (nd, nf)
    img = open(out, 'rb').read()
    got, lookup = read(img)
    want = {'/' + k: v for k, v in files.items()}
    want.update({'/f%02d' % i: bytes([i]) * (i + 1) for i in range(40)})
    assert got == want, set(got) ^ set(want)
    assert lookup(0, b'a.bin') == b'x' * 5 and lookup(0, b'f07') == bytes([7]) * 8 and lookup(0, b'nope') is None
    assert rb.hash_count(2) == 3 and rb.hash_count(10) == 11 and rb.hash_count(100) % 2 == 1 and rb.hash_count(100) % 3 != 0
print('ok')
