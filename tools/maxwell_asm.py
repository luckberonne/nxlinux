#!/usr/bin/env python3
"""A tiny Maxwell (Tegra X1) assembler for test shaders: just enough to build programs the translator (tools/dmsh_xlate) accepts, with no game material.

A graphics shader is a 0x50-byte program header (SPH) followed by 64-bit slots; every fourth slot holds a scheduling word the decoder skips. The decoder counts
addresses from the start of the code (right after the header), so the first slot is a scheduling word and the instructions sit at code offsets 8, 0x10, 0x18,
then 0x28...: three instructions per 32 bytes. Encodings follow Ryujinx's decoder tables
(third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs): the opcode in the top 12 bits, a predicate in bits 16-19 (7 = always).
"""
import struct

VERTEX, FRAGMENT = 1, 5      # ShaderStage values of the header's bits 10-13
PT = 7                       # the always-true predicate


def mov32i(rd, imm32, quad_mask=0xF):
    """MOV32I Rd, imm32 (0x010 in the top 12 bits; Rd in bits 0-7, mask 12-15, predicate 16-19, the immediate from bit 20)"""
    return (0x010 << 52) | ((imm32 & 0xFFFFFFFF) << 20) | (quad_mask << 12) | (PT << 16) | (rd & 0xFF)


def exit_():
    """EXIT (0xE30 in the top 12 bits; predicate 7; condition code T = 0xF in bits 0-4)"""
    return (0xE30 << 52) | (PT << 16) | 0xF


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def sph(stage, omap_targets=0, omap_depth=False, omap_sample_mask=False, imap=None):
    """The 0x50-byte header. omap_targets: 4 bits per render target (r, g, b, a). imap: 32 bytes, one per attribute vec4 (pixel interpolation of each component, 2 bits each)."""
    w = [0] * 20
    w[0] = 3 | (3 << 5) | (stage << 10)                   # SphType 3, Version 3
    w[18] = omap_targets
    w[19] = (1 if omap_sample_mask else 0) | (2 if omap_depth else 0)
    if imap:
        for i, b in enumerate(imap):
            w[6 + (i >> 2)] |= (b & 0xFF) << ((i & 3) * 8)
    return struct.pack('<20I', *w)


def program(instrs, header, pad_to=0x100):
    """Header, then the instructions with a scheduling word in every slot whose address has its low five bits clear, zero-padded"""
    out = bytearray(header)
    assert len(out) == 0x50
    for ins in instrs:
        if (len(out) - 0x50) % 0x20 == 0:
            out += struct.pack('<Q', 0)                   # scheduling word: the decoder skips it
        out += struct.pack('<Q', ins)
    out += bytes(max(0, pad_to - len(out)))
    return bytes(out)


def fragment_constant(r, g, b, a):
    """A fragment shader that writes one constant colour to target 0"""
    return program([mov32i(0, f32(r)), mov32i(1, f32(g)), mov32i(2, f32(b)), mov32i(3, f32(a)), exit_()], sph(FRAGMENT, omap_targets=0xF))


if __name__ == '__main__':
    import sys
    sys.stdout.buffer.write(fragment_constant(1.0, 0.5, 0.0, 1.0))
