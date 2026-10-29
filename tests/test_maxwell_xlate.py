#!/usr/bin/env python3
"""Maxwell assembler -> translator -> shader library. The assembler and packer checks are plain; the translator ones need dotnet and a built tools/dmsh_xlate
(NX_XLATE_DLL, or build/dmsh_xlate/dmsh_xlate.dll) and are skipped without them."""
import os, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools')); sys.path.insert(0, str(ROOT / 'tests'))
import maxwell_asm, shader_pack, make_maxwell_lib

DLL = Path(os.environ.get('NX_XLATE_DLL', ROOT / 'build' / 'dmsh_xlate' / 'dmsh_xlate.dll'))


class Assembler(unittest.TestCase):
    def test_scheduling_word_leads_each_group_of_four(self):
        code = maxwell_asm.fragment_constant(1, 0.5, 0, 1)
        self.assertEqual(len(code), 0x100)
        self.assertEqual(struct.unpack_from('<Q', code, 0x50)[0], 0)                       # the first slot after the header is a scheduling word
        self.assertEqual(struct.unpack_from('<Q', code, 0x50 + 8)[0] >> 52, 0x010)         # then MOV32I
        self.assertEqual(struct.unpack_from('<Q', code, 0x50 + 0x20)[0], 0)                # and again every 0x20 bytes

    def test_header_stage(self):
        w = struct.unpack_from('<20I', maxwell_asm.fragment_constant(0, 0, 0, 0))
        self.assertEqual((w[0] >> 10) & 0xF, maxwell_asm.FRAGMENT)


class Packer(unittest.TestCase):
    def test_needs_and_bindings(self):
        res = {'stage': 'ps', 'cbufs': [{'binding': 0, 'slot': 0}, {'binding': 1, 'slot': 3}, {'binding': 2, 'slot': 5}], 'textures': [{'binding': 3, 'type': 5, 'handle': 8, 'cbuf_slot': 2, 'array': 1, 'separate': False}], 'sbufs': [], 'images': []}
        lib = shader_pack.pack([shader_pack.entry('0123456789abcdef', b'\x03\x02\x23\x07' * 2, res)])
        [e] = shader_pack.unpack(lib)
        self.assertEqual(e['key'], '0123456789abcdef')
        self.assertEqual(e['stage'], 1)
        self.assertEqual(e['needs'], shader_pack.NEEDS_TEX)         # a cube map: not bindable yet
        self.assertEqual(e['binds'], [(0, shader_pack.KIND_DRV0, 0, 0), (1, shader_pack.KIND_CBUF, 0, 0), (2, shader_pack.KIND_CBUF, 2, 0)])        # c[3 + reg]

    def test_combined_2d_texture_is_a_binding(self):
        t = {'binding': 2, 'type': 3, 'handle': 8 + 4, 'cbuf_slot': 2, 'array': 1, 'separate': False}       # word 12 = byte 0x30 = c2 slot 2
        res = {'stage': 'ps', 'cbufs': [], 'textures': [t]}
        [e] = shader_pack.unpack(shader_pack.pack([shader_pack.entry('0123456789abcdef', b'\0\0\0\0', res)]))
        self.assertEqual((e['needs'], e['binds']), (0, [(2, shader_pack.KIND_TEX, 2, 3)]))
        for bad in ({'separate': True}, {'cbuf_slot': 3}, {'handle': 8 + (1 << 28)}, {'handle': 9}, {'array': 4}, {'type': 3 | 256}):
            [x] = shader_pack.unpack(shader_pack.pack([shader_pack.entry('0123456789abcdef', b'\0\0\0\0', {'stage': 'ps', 'textures': [{**t, **bad}]})]))
            self.assertEqual((x['needs'], x['binds']), (shader_pack.NEEDS_TEX, []))

    def test_bad_input(self):
        with self.assertRaises(ValueError): shader_pack.entry('00', b'\0\0\0\0', {'stage': 'ps'})
        with self.assertRaises(ValueError): shader_pack.entry('0123456789abcdef', b'\0\0\0', {'stage': 'ps'})
        with self.assertRaises(ValueError): shader_pack.unpack(b'NOPE' + bytes(8))


@unittest.skipUnless(shutil.which('dotnet') and DLL.exists(), 'dotnet or the built translator is missing')
class Translator(unittest.TestCase):
    def test_constant_fragment_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            key = make_maxwell_lib.stand_in_key()
            out = make_maxwell_lib.translate(DLL, [{'key': key, 'stage': 'ps', 'code': maxwell_asm.fragment_constant(1, 0.5, 0, 1), 'fout': {'0': 'float'}}], d)
            spv = out / f'{key}.spv'
            self.assertEqual(spv.read_bytes()[:4], struct.pack('<I', 0x07230203))
            if shutil.which('spirv-val'):
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(spv)], check=True)
            lib = Path(d) / 'm.nxsl'
            shader_pack.main(['x', str(lib), str(out)])
            [e] = shader_pack.unpack(lib.read_bytes())
            self.assertEqual((e['key'], e['stage'], e['needs'], e['binds']), (key, 1, 0, []))
            self.assertEqual(e['spirv'], spv.read_bytes())

    def test_vertex_fragment_pair(self):
        with tempfile.TemporaryDirectory() as d:
            kv, kp = make_maxwell_lib.stand_in_key(0x55, 0xF6), make_maxwell_lib.stand_in_key(0x66, 0x07)
            out = make_maxwell_lib.translate(DLL, [
                {'key': kv, 'stage': 'vs', 'code': maxwell_asm.vertex_passthrough(), 'attr': {'0': 'float', '1': 'float'}},
                {'key': kp, 'stage': 'ps', 'code': maxwell_asm.fragment_varying(), 'fout': {'0': 'float'}}], d)
            for k in (kv, kp):
                if shutil.which('spirv-val'): subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(out / f'{k}.spv')], check=True)
            vs = (out / f'{kv}.spv').read_bytes()
            self.assertNotIn(b'gl_CullDistance', vs)            # the patched per-vertex block has no cull member (Vulkan caps clip + cull sizes together)

    def test_driver_segments_and_support_buffer_are_bindings(self):
        with tempfile.TemporaryDirectory() as d:
            k1, k2 = make_maxwell_lib.stand_in_key(0x99, 0x3A), make_maxwell_lib.stand_in_key()
            out = make_maxwell_lib.translate(DLL, [
                {'key': k1, 'stage': 'ps', 'code': maxwell_asm.fragment_drivers(), 'fout': {'0': 'float'}},
                {'key': k2, 'stage': 'ps', 'code': maxwell_asm.fragment_constant(1, 0.5, 0, 1), 'fout': {'0': 'float'}}], d)
            lib = Path(d) / 'm.nxsl'
            shader_pack.main(['x', str(lib), str(out)])
            e = {x['key']: x for x in shader_pack.unpack(lib.read_bytes())}
            self.assertEqual(sorted(b[1] for b in e[k1]['binds']), [shader_pack.KIND_SUPPORT, shader_pack.KIND_DRV0, shader_pack.KIND_DRV2])
            self.assertEqual(e[k1]['needs'], 0)
            self.assertEqual(e[k2]['binds'], [])                    # declares the support buffer but never reads it

    def test_stand_in_key_is_stable(self):
        self.assertEqual(len(make_maxwell_lib.stand_in_key()), 16)
        self.assertNotEqual(make_maxwell_lib.stand_in_key(), make_maxwell_lib.stand_in_key(fill_code=0xE6))


if __name__ == '__main__':
    unittest.main()
