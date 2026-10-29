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


def _attr_access(opcode13, reg, rb, offset, size, extra=0):
    return (opcode13 << 51) | (extra) | ((size & 3) << 47) | ((offset & 0x7FF) << 20) | (PT << 16) | (255 << 8) | (reg & 0xFF)


def ald(rd, offset, comps=4):
    """ALD Rd.., a[offset]: loads `comps` (1-4) consecutive 32-bit components of an input attribute (0x70 = position, 0x80 + 16 n = generic attribute n).
    0xEFD8 << 48 in the table: 13 bits 1110111111011; Rd bits 0-7, the register holding the address (RZ here) bits 8-15, the byte offset bits 20-30, size bits 47-48."""
    return _attr_access(0b1110111111011, rd, 0, offset, comps - 1)


def ast(rs, offset, comps=4):
    """AST a[offset], Rs..: stores `comps` consecutive registers to an output attribute (13 bits 1110111111110; the data register is in bits 0-7)"""
    return _attr_access(0b1110111111110, rs, 0, offset, comps - 1)


def ipa(rd, offset):
    """IPA Rd, a[offset]: a fragment shader's interpolated input (8 bits 11100000; the byte offset in bits 28-37; mode Pass; RZ as the other operand registers)"""
    return (0xE0 << 56) | ((offset & 0x3FF) << 28) | (255 << 20) | (PT << 16) | (255 << 8) | (rd & 0xFF)


def ldc(rd, slot, byte_offset):
    """LDC Rd, c[slot][offset]: one 32-bit word of a constant buffer (13 bits 1110111110010; slot bits 36-40, byte offset bits 20-35, size B32 = 4 in bits 48-50,
    address mode IA = 0, the address register is RZ). Slots 0-2 belong to the driver; an NVN uniform buffer n is slot 3 + n."""
    return (0b1110111110010 << 51) | (4 << 48) | ((slot & 0x1F) << 36) | ((byte_offset & 0xFFFF) << 20) | (PT << 16) | (255 << 8) | (rd & 0xFF)


def fadd(rd, ra, rb):
    """FADD Rd, Ra, Rb (13 bits 0101110001011; Rb bits 20-27; no modifiers)"""
    return (0b0101110001011 << 51) | ((rb & 0xFF) << 20) | (PT << 16) | ((ra & 0xFF) << 8) | (rd & 0xFF)


def tex(rd, ra, slot, dim=2, lod=1):
    """TEX Rd.., Ra.., (texture slot): samples a combined texture, the coordinates in Ra, Ra+1 (...), all four components out to Rd..Rd+3 (5 bits 11000; the texture
    handle word index in bits 36-48: driver segment c[2] holds combined handles from byte 0x20, 8 bytes per slot; component mask bits 31-34; dimension bits 28-30
    (2 = 2D); level of detail mode bits 55-57, 1 = LZ: the base level)."""
    tid = (0x20 + 8 * slot) // 4
    return (0b11000 << 59) | ((lod & 7) << 55) | (tid << 36) | (0xF << 31) | ((dim & 7) << 28) | (255 << 20) | (PT << 16) | ((ra & 0xFF) << 8) | (rd & 0xFF)


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


def vertex_passthrough():
    """A vertex shader: attribute 0 (x, y, z, w) is the position, attribute 1 goes out as generic output 0 (the colour)"""
    return program([ald(0, 0x80), ald(4, 0x90), ast(0, 0x70), ast(4, 0x80), exit_()], sph(VERTEX))


def vertex_shifted(slot=3):
    """Like vertex_passthrough, but the position is (x + c[slot][0], y + c[slot][4], c[slot][8], 1): the vertex uniform buffer of tests/shaders/draw.vert"""
    return program([ald(0, 0x80), ald(4, 0x90), ldc(8, slot, 0), ldc(9, slot, 4), ldc(10, slot, 8),
                    fadd(0, 0, 8), fadd(1, 1, 9), fadd(2, 10, 255), ast(0, 0x70), ast(4, 0x80), exit_()], sph(VERTEX))


def fragment_varying():
    """A fragment shader that writes generic input 0, perspective-interpolated, to target 0"""
    return program([ipa(0, 0x80), ipa(1, 0x84), ipa(2, 0x88), ipa(3, 0x8C), exit_()], sph(FRAGMENT, omap_targets=0xF, imap=[0xAA]))


def fragment_textured(slot=0):
    """A fragment shader that samples texture `slot` at the coordinates in generic input 0 (x, y) and writes it to target 0"""
    return program([ipa(4, 0x80), ipa(5, 0x84), tex(0, 4, slot), exit_()], sph(FRAGMENT, omap_targets=0xF, imap=[0xAA]))


if __name__ == '__main__':
    import sys
    sys.stdout.buffer.write(fragment_constant(1.0, 0.5, 0.0, 1.0))
