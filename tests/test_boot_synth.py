#!/usr/bin/env python3
"""The whole load path on a made-up title, with no game material: the NVN draw guest is linked as an NSO (tools/synth_guest.ld, tools/synth_mod0.S), packed by
tools/synth_nsp.py into an NSP with throwaway keys, extracted by the vendored nx_extract (NCA decryption, PFS0, ExeFS), unpacked by tools/nso.py into its memory image,
mapped by the host loader through its MOD0, relocated against the HLE stubs and run on the JIT: tests/test_nvn_draw.cpp then checks the pixels the GPU drew.

Usage: test_boot_synth.py <test_nvn_draw binary> <build dir>      (skipped when clang, the .venv with cryptography or work/nx_extract is missing)"""
import os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'tools'))
import nso

HOST, BUILD = (Path(a) for a in sys.argv[1:3]) if len(sys.argv) >= 3 else (None, None)
VENV_PY = REPO / '.venv' / 'bin' / 'python'
NX = REPO / 'work' / 'nx_extract' / 'nx_extract'


def venv_has_cryptography():
    return VENV_PY.is_file() and subprocess.run([str(VENV_PY), '-c', 'import cryptography'], capture_output=True).returncode == 0


@unittest.skipUnless(HOST and HOST.is_file(), 'no test_nvn_draw binary given')
@unittest.skipUnless(shutil.which('clang') and shutil.which('ld.lld'), 'clang or lld is missing')
@unittest.skipUnless(venv_has_cryptography(), '.venv with cryptography is missing')
@unittest.skipUnless(NX.is_file(), 'work/nx_extract is not built')
class BootSynthetic(unittest.TestCase):
    def test_draw_guest_from_a_synthetic_nsp(self):
        with tempfile.TemporaryDirectory(prefix='boot_synth_') as d:
            d = Path(d)
            elf = d / 'draw.elf'
            nvn = REPO / 'src' / 'gfx' / 'nvn'
            subprocess.run(['clang', '--target=aarch64-linux-musl', '-ffreestanding', '-O1', '-fPIC', '-shared', '-nostdlib', '-nostartfiles', '-fuse-ld=lld',
                            f'-I{nvn}', f'-I{nvn / "include"}', '-Wl,-T,' + str(REPO / 'tools' / 'synth_guest.ld') + ',-z,max-page-size=0x1000',
                            '-o', str(elf), str(REPO / 'tests' / 'guest' / 'nvn_draw.c'), str(REPO / 'tools' / 'synth_mod0.S')], check=True)
            subprocess.run([str(VENV_PY), str(REPO / 'tools' / 'synth_nsp.py'), str(d / 'title'), '--main-elf', str(elf)], check=True, capture_output=True)
            subprocess.run([str(NX), str(d / 'title' / 'prod.keys'), str(d / 'title' / 'base.nsp'), '-', str(d / 'out')], check=True, capture_output=True, timeout=180)
            main = d / 'out' / 'ExeFS' / 'main'
            self.assertTrue(main.is_file())
            m = nso.read(main)
            img = d / 'main.img'
            img.write_bytes(bytes(m.image))
            with open(img, 'r+b') as f: f.truncate(len(m.image) + m.bss)
            args = [str(HOST), str(img), str(BUILD / 'shaders' / 'draw.vert.spv'), str(BUILD / 'shaders' / 'draw.frag.spv')]
            lib = BUILD / 'maxwell.nxsl'
            if lib.is_file(): args.append(str(lib))
            p = subprocess.run(args, capture_output=True, text=True, timeout=120, env={**os.environ, 'NX_IMG_BSS': str(m.bss)})
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn('PASS', p.stdout)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
