#include "bc3enc.h"
#include <string.h>

static uint16_t pack565(int r, int g, int b) { return (uint16_t)((r * 31 + 127) / 255 << 11 | (g * 63 + 127) / 255 << 5 | (b * 31 + 127) / 255); }
static void unpack565(uint16_t c, int rgb[3]) {
    int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    rgb[0] = r << 3 | r >> 2; rgb[1] = g << 2 | g >> 4; rgb[2] = b << 3 | b >> 2;
}

static void encode_block(const uint8_t px[16][4], uint8_t out[16]) {
    // alpha: BC4 with a0 > a1, eight values
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) { if (px[i][3] < amin) amin = px[i][3]; if (px[i][3] > amax) amax = px[i][3]; }
    int pal[8]; pal[0] = amax; pal[1] = amin;
    if (amax == amin) { for (int k = 2; k < 8; k++) pal[k] = amax; }
    else for (int k = 1; k < 7; k++) pal[k + 1] = ((7 - k) * amax + k * amin) / 7;
    uint64_t abits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; k++) { int d = px[i][3] - pal[k]; d *= d; if (d < bd) { bd = d; best = k; } }
        abits |= (uint64_t)best << (3 * i);
    }
    out[0] = (uint8_t)amax; out[1] = (uint8_t)amin;
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(abits >> (8 * i));
    // colour: the bounding box, nudged inwards by 1/16 of its extent, as endpoints; four-colour mode (c0 > c1)
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) { if (px[i][c] < lo[c]) lo[c] = px[i][c]; if (px[i][c] > hi[c]) hi[c] = px[i][c]; }
    for (int c = 0; c < 3; c++) { int inset = (hi[c] - lo[c]) >> 4; lo[c] += inset; hi[c] -= inset; }
    uint16_t c0 = pack565(hi[0], hi[1], hi[2]), c1 = pack565(lo[0], lo[1], lo[2]);
    uint32_t idx = 0;
    if (c0 == c1) { if (c0 < 0xffff) c0++; else c1--; }      // equal endpoints would select the three-colour mode
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    int p[4][3], e0[3], e1[3]; unpack565(c0, e0); unpack565(c1, e1);
    for (int c = 0; c < 3; c++) { p[0][c] = e0[c]; p[1][c] = e1[c]; p[2][c] = (2 * e0[c] + e1[c]) / 3; p[3][c] = (e0[c] + 2 * e1[c]) / 3; }
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 4; k++) {
            int d = 0; for (int c = 0; c < 3; c++) { int x = px[i][c] - p[k][c]; d += x * x; }
            if (d < bd) { bd = d; best = k; }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    out[8] = (uint8_t)c0; out[9] = (uint8_t)(c0 >> 8); out[10] = (uint8_t)c1; out[11] = (uint8_t)(c1 >> 8);
    out[12] = (uint8_t)idx; out[13] = (uint8_t)(idx >> 8); out[14] = (uint8_t)(idx >> 16); out[15] = (uint8_t)(idx >> 24);
}

void bc3_encode_image(const uint8_t *rgba, int w, int h, uint8_t *out) {
    int bx = (w + 3) / 4, by = (h + 3) / 4;
    for (int y = 0; y < by; y++) for (int x = 0; x < bx; x++) {
        uint8_t px[16][4];
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
            int sx = x * 4 + i < w ? x * 4 + i : w - 1, sy = y * 4 + j < h ? y * 4 + j : h - 1;      // an edge block repeats the last row and column
            memcpy(px[j * 4 + i], rgba + ((size_t)sy * w + sx) * 4, 4);
        }
        encode_block(px, out + ((size_t)y * bx + x) * 16);
    }
}
