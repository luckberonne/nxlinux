#!/usr/bin/env python3
"""Disassemble extracted Ounce programs (tools/ounce_extract.py output) with nvdisasm, for tools/ounce_xlate.

  ounce_sass.py DIR [--limit N] [--jobs N]        writes DIR/<key>.sass for every DIR/<key>.code that has none

The Sm86 frontend of the translator reads the text nvdisasm prints for Ampere (SM86) machine code. nvdisasm only reads ELF files, so each program's instructions (the code area
minus its header: 0x80 bytes, none for a compute program) are put into the .text of a cubin that nvcc built once, padded with NOPs. This needs the CUDA toolkit (nvcc and nvdisasm, CUDA_HOME or /opt/cuda);
a decoder of its own that does not need it is the way to drop that dependency."""
import argparse, multiprocessing as mp, os, shutil, subprocess, sys, tempfile
from pathlib import Path

HEADER = 0x80
HEADER_COMPUTE = 0         # a compute program has no header at all: its code area is instructions from byte 0 (S2R of the thread ids first). A graphics program starts with an SPH whose word 0 & 0x3ff is 0x81 (vertex, mesh) or 0x82 (fragment)
NOP = bytes.fromhex('18790000000000000000000000c00f00')       # NOP, the filler after the program


def cuda_tool(name):
    for d in (os.environ.get('CUDA_HOME'), '/opt/cuda', '/usr/local/cuda'):
        if d and (Path(d) / 'bin' / name).is_file(): return str(Path(d) / 'bin' / name)
    p = shutil.which(name)
    if not p: sys.exit(f'{name} not found: install the CUDA toolkit (CUDA_HOME)')
    return p


def make_template(work, nbytes):
    """A cubin whose kernel's .text is at least nbytes: its (offset, size) in the file."""
    n = nbytes // 16 + 64        # about one instruction per line of the kernel
    src = work / 'k.cu'
    src.write_text('__global__ void k(float*a){float x=a[0];' + ''.join(f'x=x*{1.0 + (i % 97) * 0.001}f+a[{i % 64 + 1}];' for i in range(n)) + 'a[0]=x;}')
    subprocess.run([cuda_tool('nvcc'), '-arch=sm_86', '-cubin', '-O1', str(src), '-o', str(work / 'k.cubin')], check=True)
    t = (work / 'k.cubin').read_bytes()
    out = subprocess.run(['readelf', '-S', '-W', str(work / 'k.cubin')], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if '.text._Z1kPf' in line:
            f = line.split(']')[1].split()
            return t, int(f[3], 16), int(f[4], 16)
    sys.exit('no .text in the template')


_G = {}


def init(template, off, size, nvdisasm):
    _G.update(t=template, off=off, size=size, nvdisasm=nvdisasm)


def one(path):
    out = path.with_suffix('.sass')
    if out.exists(): return 0
    raw = path.read_bytes()
    code = raw[HEADER if (int.from_bytes(raw[:4], 'little') & 0x3ff) in (0x81, 0x82) else HEADER_COMPUTE:]
    if len(code) > _G['size']: return 1
    t = bytearray(_G['t']); t[_G['off']:_G['off'] + _G['size']] = code + NOP * ((_G['size'] - len(code)) // 16)
    with tempfile.NamedTemporaryFile(suffix='.cubin') as f:
        f.write(t); f.flush()
        r = subprocess.run([_G['nvdisasm'], '-c', '-raw', f.name], capture_output=True, text=True)
    s = r.stdout; i = s.find('.text._Z1kPf:')
    if i < 0 or 'Illegal' in r.stderr + s: return 1
    out.write_text(s[i:])
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dir'); ap.add_argument('--limit', type=int, default=1 << 30); ap.add_argument('--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()
    files = sorted(Path(a.dir).glob('*.code'))[:a.limit]
    if not files: sys.exit('no .code files')
    biggest = max(f.stat().st_size for f in files)
    with tempfile.TemporaryDirectory() as w:
        tpl, off, size = make_template(Path(w), biggest)
        with mp.Pool(a.jobs, init, (tpl, off, size, cuda_tool('nvdisasm'))) as p:
            bad = sum(p.imap_unordered(one, files, chunksize=8))
    print(f'{len(files)} programs, {bad} not disassembled')


if __name__ == '__main__':
    main()
