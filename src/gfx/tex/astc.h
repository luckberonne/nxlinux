// ASTC LDR decoding in software, for devices whose GPU has no ASTC (desktop AMD): blocks of 16 bytes, 2D, 4x4 to 12x12, any partition count, dual plane, every LDR colour endpoint mode.
// HDR endpoint modes and illegal encodings decode to the error colour (magenta), as the standard says. The unquantisation of trit and quint values is a rounded rescale, not the standard's
// table: the same values within a unit, which no 8-bit output shows.
#pragma once
#include <stdint.h>

// One block → bw*bh RGBA8, row-major. sRGB textures decode the same way: the endpoints are sRGB-encoded 8-bit values and the sampler converts.
void astc_decode_block(const uint8_t block[16], int bw, int bh, uint8_t *rgba);
// A whole level: the blocks in row-major order, ceil(w/bw) * ceil(h/bh) of them → w*h RGBA8 (the partial blocks at the right and bottom edges are cropped).
void astc_decode_image(const uint8_t *blocks, int w, int h, int bw, int bh, uint8_t *rgba);
