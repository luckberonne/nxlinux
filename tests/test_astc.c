// The ASTC decoder against blocks built by hand: a void-extent block, a 4x4 block with one partition and direct RGB endpoints (the weight grid, the 2-bit weights stored from the end of
// the block, the 8-bit colour values), and the error colour of an HDR block.
#include "../src/gfx/tex/astc.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static void put(uint8_t *b, int pos, int n, unsigned v) { for (int i = 0; i < n; i++) { int p = pos + i; b[p >> 3] = (uint8_t)((b[p >> 3] & ~(1 << (p & 7))) | (((v >> i) & 1) << (p & 7))); } }

int main(void) {
    uint8_t blk[16], px[12 * 12 * 4];
    // void extent: block mode 0x1fc, then R, G, B, A as 16 bits
    memset(blk, 0, 16); put(blk, 0, 11, 0x1fc);
    uint16_t c[4] = {0x1234, 0x5678, 0x9abc, 0xdef0}; memcpy(blk + 8, c, 8);
    astc_decode_block(blk, 6, 6, px);
    for (int i = 0; i < 36; i++) CHECK(px[4 * i] == 0x12 && px[4 * i + 1] == 0x56 && px[4 * i + 2] == 0x9a && px[4 * i + 3] == 0xde);
    // 4x4, grid 4x4, 2-bit weights, one partition, CEM 8 (RGB direct), eight-bit endpoints
    memset(blk, 0, 16);
    put(blk, 0, 11, 0x42);                               // block mode: 4x4 weights of 4 levels (R = 4: bits 1:0 = 2), no dual plane
    put(blk, 11, 2, 0);                                  // one partition
    put(blk, 13, 4, 8);                                  // CEM 8
    const int v[6] = {0, 255, 0, 128, 0, 64};            // e0 = (0,0,0), e1 = (255,128,64)
    for (int i = 0; i < 6; i++) put(blk, 17 + 8 * i, 8, (unsigned)v[i]);
    for (int i = 0; i < 16; i++) { unsigned q = (unsigned)(i & 3); put(blk, 127 - 2 * i, 1, q & 1); put(blk, 126 - 2 * i, 1, q >> 1); }   // weight i: LSB at bit 127-2i, MSB below it
    astc_decode_block(blk, 4, 4, px);
    static const int wq[4] = {0, 21, 43, 64};            // 2-bit weights unquantised: 0, 21, 42 (+1 above 32), 63 (+1)
    for (int i = 0; i < 16; i++) {
        int w = wq[i & 3], e1[3] = {255, 128, 64};
        for (int ch = 0; ch < 3; ch++) CHECK(px[4 * i + ch] == (uint8_t)(((((e1[ch] << 8) | e1[ch]) * w + 32) >> 6) >> 8));
        CHECK(px[4 * i + 3] == 255);
    }
    CHECK(px[0] == 0 && px[4 * 3] == 255);               // the first texel has weight 0 (e0), the fourth weight 64 (e1)
    // an HDR endpoint mode (CEM 2) is the error colour
    memset(blk, 0, 16); put(blk, 0, 11, 0x42); put(blk, 13, 4, 2);
    astc_decode_block(blk, 4, 4, px);
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 255 && px[3] == 255);
    // an image crops the partial blocks: 5x5 in 4x4 blocks is 2x2 blocks
    uint8_t img[4 * 16], out[5 * 5 * 4];
    for (int b = 0; b < 4; b++) { memset(img + 16 * b, 0, 16); put(img + 16 * b, 0, 11, 0x1fc); uint16_t cc[4] = {(uint16_t)(0x1000 * (b + 1)), 0, 0, 0xffff}; memcpy(img + 16 * b + 8, cc, 8); }
    astc_decode_image(img, 5, 5, 4, 4, out);
    CHECK(out[0] == 0x10 && out[4 * 4] == 0x20 && out[4 * 5 * 4] == 0x30 && out[4 * (4 * 5 + 4)] == 0x40);
    printf(g_fail ? "FAIL\n" : "ok\n");
    return g_fail;
}
