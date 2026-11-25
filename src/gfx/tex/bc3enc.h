#pragma once
#include <stddef.h>
#include <stdint.h>
// A fast BC3 (DXT5) encoder for RGBA8 images: the device holds an ASTC texture as BC3 (1 byte a pixel) rather than RGBA8 (4 bytes). Colour uses the bounding-box endpoints of each 4x4 block and picks
// the nearest of the four palette entries per pixel; alpha uses the block's min and max with the eight-value interpolation. `out` takes ((w+3)/4) * ((h+3)/4) * 16 bytes.
void bc3_encode_image(const uint8_t *rgba, int w, int h, uint8_t *out);
