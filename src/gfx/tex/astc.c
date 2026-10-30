#include "astc.h"
#include <string.h>

typedef struct { uint8_t kind, bits; uint16_t levels; } quant_t;          // kind 0: bits only, 1: trits, 2: quints; levels: how many values it takes
static const quant_t QUANT[21] = {
    {0, 1, 2}, {1, 0, 3}, {0, 2, 4}, {2, 0, 5}, {1, 1, 6}, {0, 3, 8}, {2, 1, 10}, {1, 2, 12}, {0, 4, 16}, {2, 2, 20}, {1, 3, 24},
    {0, 5, 32}, {2, 3, 40}, {1, 4, 48}, {0, 6, 64}, {2, 4, 80}, {1, 5, 96}, {0, 7, 128}, {2, 5, 160}, {1, 6, 192}, {0, 8, 256},
};

static int ise_bits(int n, int q) {                                       // bits an integer sequence of n values takes
    const quant_t *Q = &QUANT[q];
    return Q->kind == 0 ? n * Q->bits : Q->kind == 1 ? (8 * n + 4) / 5 + n * Q->bits : (7 * n + 2) / 3 + n * Q->bits;
}

// A stream of bits, one per byte: s[i] is the i-th bit. Reads b bits LSB first.
static unsigned rd(const uint8_t *s, int *pos, int b) { unsigned v = 0; for (int i = 0; i < b; i++) v |= (unsigned)s[(*pos)++] << i; return v; }

static void trits(unsigned T, int t[5]) {
    unsigned C, t4, t3, t2, t1, t0;
    if (((T >> 2) & 7) == 7) { C = (((T >> 5) & 7) << 2) | (T & 3); t4 = 2; t3 = 2; }
    else {
        C = T & 31;
        if (((T >> 5) & 3) == 3) { t4 = 2; t3 = (T >> 7) & 1; } else { t4 = (T >> 7) & 1; t3 = (T >> 5) & 3; }
    }
    if ((C & 3) == 3) { t2 = 2; t1 = (C >> 4) & 1; t0 = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1) & 1); }
    else if (((C >> 2) & 3) == 3) { t2 = 2; t1 = 2; t0 = C & 3; }
    else { t2 = (C >> 4) & 1; t1 = (C >> 2) & 3; t0 = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1) & 1); }
    t[0] = (int)t0; t[1] = (int)t1; t[2] = (int)t2; t[3] = (int)t3; t[4] = (int)t4;
}
static void quints(unsigned Q, int q[3]) {
    unsigned q2, q1, q0, C;
    if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
        q2 = ((Q & 1) << 2) | ((((Q >> 4) & 1) & ~(Q & 1) & 1) << 1) | (((Q >> 3) & 1) & ~(Q & 1) & 1); q1 = 4; q0 = 4;
    } else {
        if (((Q >> 1) & 3) == 3) { q2 = 4; C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1); }
        else { q2 = (Q >> 5) & 3; C = Q & 31; }
        if ((C & 7) == 5) { q1 = 4; q0 = (C >> 3) & 3; } else { q1 = (C >> 3) & 3; q0 = C & 7; }
    }
    q[0] = (int)q0; q[1] = (int)q1; q[2] = (int)q2;
}

// Decodes n values of quantisation q from the bit stream s starting at *pos; out[i] is the index in 0 .. levels-1.
static void ise_decode(const uint8_t *s, int pos, int n, int q, uint8_t *out) {
    const quant_t *Q = &QUANT[q]; int b = Q->bits;
    if (Q->kind == 0) { for (int i = 0; i < n; i++) out[i] = (uint8_t)rd(s, &pos, b); return; }
    if (Q->kind == 1) {
        for (int g = 0; g < n; g += 5) {
            unsigned m[5] = {0}, T = 0; int cnt = n - g < 5 ? n - g : 5;
            for (int i = 0; i < cnt; i++) {
                m[i] = rd(s, &pos, b);
                static const uint8_t nb[5] = {2, 2, 1, 2, 1}, sh[5] = {0, 2, 4, 5, 7};
                T |= rd(s, &pos, nb[i]) << sh[i];
            }
            int t[5]; trits(T, t);
            for (int i = 0; i < cnt; i++) out[g + i] = (uint8_t)((t[i] << b) | (int)m[i]);
        }
        return;
    }
    for (int g = 0; g < n; g += 3) {
        unsigned m[3] = {0}, Qv = 0; int cnt = n - g < 3 ? n - g : 3;
        for (int i = 0; i < cnt; i++) {
            m[i] = rd(s, &pos, b);
            static const uint8_t nb[3] = {3, 2, 2}, sh[3] = {0, 3, 5};
            Qv |= rd(s, &pos, nb[i]) << sh[i];
        }
        int qq[3]; quints(Qv, qq);
        for (int i = 0; i < cnt; i++) out[g + i] = (uint8_t)((qq[i] << b) | (int)m[i]);
    }
}

static int unq_color(int v, int q) {                                       // an endpoint value index → 0..255
    const quant_t *Q = &QUANT[q];
    if (Q->kind == 0) {
        switch (Q->bits) {
        case 1: return v * 255; case 2: return v * 85; case 3: return (v << 5) | (v << 2) | (v >> 1); case 4: return v * 17;
        case 5: return (v << 3) | (v >> 2); case 6: return (v << 2) | (v >> 4); case 7: return (v << 1) | (v >> 6); default: return v;
        }
    }
    return (v * 255 + (Q->levels - 1) / 2) / (Q->levels - 1);
}
static int unq_weight(int v, int q) {                                      // a weight index → 0..64
    const quant_t *Q = &QUANT[q]; int w;
    if (Q->kind == 0) {
        switch (Q->bits) { case 1: w = v * 63; break; case 2: w = v * 21; break; case 3: w = v * 9; break; case 4: w = (v << 2) | (v >> 2); break; default: w = (v << 1) | (v >> 4); }
    } else w = (v * 63 + (Q->levels - 1) / 2) / (Q->levels - 1);
    return w > 32 ? w + 1 : w;
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void bts(int *a, int *b) {                                          // bit_transfer_signed: *a is the offset, *b the base
    int va = *a, vb = *b;
    vb = (vb >> 1) | (va & 0x80); va = (va >> 1) & 0x3f; if (va & 0x20) va -= 0x40;
    *a = va; *b = vb;
}
static void contract(int *c) { c[0] = (c[0] + c[2]) >> 1; c[1] = (c[1] + c[2]) >> 1; }

// The two endpoints (RGBA, 0..255) of a colour endpoint mode from its values; 0 for an HDR mode.
static int endpoints(int cem, const int *v, int e0[4], int e1[4]) {
    switch (cem) {
    case 0: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = e1[3] = 255; return 1;
    case 1: { int l0 = (v[0] >> 2) | (v[1] & 0xc0), l1 = clamp255(l0 + (v[1] & 0x3f)); e0[0] = e0[1] = e0[2] = l0; e1[0] = e1[1] = e1[2] = l1; e0[3] = e1[3] = 255; return 1; }
    case 4: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = v[2]; e1[3] = v[3]; return 1;
    case 5: { int a0 = v[1], b0 = v[0], a1 = v[3], b1 = v[2]; bts(&a0, &b0); bts(&a1, &b1);
              e0[0] = e0[1] = e0[2] = b0; e1[0] = e1[1] = e1[2] = clamp255(b0 + a0); e0[3] = b1; e1[3] = clamp255(b1 + a1); return 1; }
    case 6: e0[0] = (v[0] * v[3]) >> 8; e0[1] = (v[1] * v[3]) >> 8; e0[2] = (v[2] * v[3]) >> 8; e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e0[3] = e1[3] = 255; return 1;
    case 8: case 12: {
        int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        int a0 = cem == 12 ? v[6] : 255, a1 = cem == 12 ? v[7] : 255;
        if (s1 >= s0) { e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e0[3] = a0; e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; e1[3] = a1; }
        else { e0[0] = v[1]; e0[1] = v[3]; e0[2] = v[5]; contract(e0); e0[3] = a1; e1[0] = v[0]; e1[1] = v[2]; e1[2] = v[4]; contract(e1); e1[3] = a0; }
        return 1;
    }
    case 9: case 13: {
        int a[4] = {v[1], v[3], v[5], cem == 13 ? v[7] : 0}, b[4] = {v[0], v[2], v[4], cem == 13 ? v[6] : 255};
        for (int i = 0; i < (cem == 13 ? 4 : 3); i++) bts(&a[i], &b[i]);
        int base[4] = {b[0], b[1], b[2], b[3]}, top[4] = {clamp255(b[0] + a[0]), clamp255(b[1] + a[1]), clamp255(b[2] + a[2]), cem == 13 ? clamp255(b[3] + a[3]) : 255};
        if (a[0] + a[1] + a[2] >= 0) { memcpy(e0, base, sizeof base); memcpy(e1, top, sizeof top); }
        else { int t0[4] = {b[0] + a[0], b[1] + a[1], b[2] + a[2], top[3]}, t1[4] = {b[0], b[1], b[2], base[3]};
               for (int i = 0; i < 3; i++) t0[i] = clamp255(t0[i]);
               contract(t0); contract(t1); memcpy(e0, t0, sizeof t0); memcpy(e1, t1, sizeof t1); }
        return 1;
    }
    case 10: e0[0] = (v[0] * v[3]) >> 8; e0[1] = (v[1] * v[3]) >> 8; e0[2] = (v[2] * v[3]) >> 8; e0[3] = v[4]; e1[0] = v[0]; e1[1] = v[1]; e1[2] = v[2]; e1[3] = v[5]; return 1;
    default: return 0;                                                      // 2, 3, 7, 11, 14, 15: HDR
    }
}

static uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4; p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3; p ^= p << 6; p ^= p >> 17;
    return p;
}
static int partition_of(int seed, int x, int y, int count, int small) {
    if (small) { x <<= 1; y <<= 1; }
    seed += (count - 1) * 1024;
    uint32_t r = hash52((uint32_t)seed);
    int s1 = r & 15, s2 = (r >> 4) & 15, s3 = (r >> 8) & 15, s4 = (r >> 12) & 15, s5 = (r >> 16) & 15, s6 = (r >> 20) & 15, s7 = (r >> 24) & 15, s8 = (r >> 28) & 15;
    int s9 = (r >> 18) & 15, s10 = (r >> 22) & 15, s11 = (r >> 26) & 15, s12 = ((r >> 30) | (r << 2)) & 15;
    s1 *= s1; s2 *= s2; s3 *= s3; s4 *= s4; s5 *= s5; s6 *= s6; s7 *= s7; s8 *= s8; s9 *= s9; s10 *= s10; s11 *= s11; s12 *= s12;
    int sh1, sh2, sh3;
    if (seed & 1) { sh1 = seed & 2 ? 4 : 5; sh2 = count == 3 ? 6 : 5; } else { sh1 = count == 3 ? 6 : 5; sh2 = seed & 2 ? 4 : 5; }
    sh3 = seed & 0x10 ? sh1 : sh2;
    s1 >>= sh1; s2 >>= sh2; s3 >>= sh1; s4 >>= sh2; s5 >>= sh1; s6 >>= sh2; s7 >>= sh1; s8 >>= sh2; s9 >>= sh3; s10 >>= sh3; s11 >>= sh3; s12 >>= sh3;
    int a = (s1 * x + s2 * y + (int)(r >> 14)) & 0x3f, b = (s3 * x + s4 * y + (int)(r >> 10)) & 0x3f;
    int c = (s5 * x + s6 * y + (int)(r >> 6)) & 0x3f, d = (s7 * x + s8 * y + (int)(r >> 2)) & 0x3f;
    (void)s9; (void)s10; (void)s11; (void)s12;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

static void error_block(int bw, int bh, uint8_t *rgba) { for (int i = 0; i < bw * bh; i++) { rgba[4 * i] = 255; rgba[4 * i + 1] = 0; rgba[4 * i + 2] = 255; rgba[4 * i + 3] = 255; } }

void astc_decode_block(const uint8_t blk[16], int bw, int bh, uint8_t *rgba) {
    uint8_t fwd[128], rev[128];
    for (int i = 0; i < 128; i++) fwd[i] = (blk[i >> 3] >> (i & 7)) & 1;
    for (int i = 0; i < 128; i++) rev[i] = fwd[127 - i];
    int pos = 0;
    unsigned mode = rd(fwd, &pos, 11);
    if ((mode & 0x1ff) == 0x1fc) {                                           // void extent: one colour
        if (mode & 0x200) { error_block(bw, bh, rgba); return; }              // HDR
        const uint16_t *c = (const uint16_t *)(blk + 8); uint8_t px[4] = {(uint8_t)(c[0] >> 8), (uint8_t)(c[1] >> 8), (uint8_t)(c[2] >> 8), (uint8_t)(c[3] >> 8)};
        for (int i = 0; i < bw * bh; i++) memcpy(rgba + 4 * i, px, 4);
        return;
    }
    // block mode → weight grid
    unsigned base = (mode >> 4) & 1, H = (mode >> 9) & 1, D = (mode >> 10) & 1, A = (mode >> 5) & 3, xw, yw;
    if (mode & 3) {
        base |= (mode & 3) << 1; unsigned B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
        case 0: xw = B + 4; yw = A + 2; break;
        case 1: xw = B + 8; yw = A + 2; break;
        case 2: xw = A + 2; yw = B + 8; break;
        default: B &= 1; if (mode & 0x100) { xw = B + 2; yw = A + 2; } else { xw = A + 2; yw = B + 6; }
        }
    } else {
        base |= ((mode >> 2) & 3) << 1;
        if (((mode >> 2) & 3) == 0) { error_block(bw, bh, rgba); return; }
        unsigned B = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
        case 0: xw = 12; yw = A + 2; break;
        case 1: xw = A + 2; yw = 12; break;
        case 2: xw = A + 6; yw = B + 6; D = 0; H = 0; break;
        default:
            if (A == 0) { xw = 6; yw = 10; } else if (A == 1) { xw = 10; yw = 6; } else { error_block(bw, bh, rgba); return; }
        }
    }
    int wq = (int)(base - 2) + 6 * (int)H, nweights = (int)(xw * yw * (D + 1));
    int wbits = ise_bits(nweights, wq);
    if (wq < 0 || wq > 11 || nweights > 64 || wbits < 24 || wbits > 96 || (int)xw > bw || (int)yw > bh) { error_block(bw, bh, rgba); return; }
    int nparts = (int)rd(fwd, &pos, 2) + 1;
    if (nparts == 4 && D) { error_block(bw, bh, rgba); return; }
    int seed = 0, cem[4] = {0}, cstart, high = 0;
    if (nparts == 1) { cem[0] = (int)rd(fwd, &pos, 4); cstart = 17; }
    else {
        seed = (int)rd(fwd, &pos, 10);
        unsigned enc = rd(fwd, &pos, 6); cstart = 29;
        if ((enc & 3) == 0) { for (int i = 0; i < nparts; i++) cem[i] = (int)((enc >> 2) & 15); }
        else {
            high = 3 * nparts - 4;
            int hp = 128 - wbits - high; unsigned hv = 0; for (int i = 0; i < high; i++) hv |= (unsigned)fwd[hp + i] << i;
            enc |= hv << 6;
            int bc = (int)(enc & 3) - 1;
            for (int i = 0; i < nparts; i++) cem[i] = ((bc + (int)((enc >> (2 + i)) & 1)) << 2) | (int)((enc >> (2 + nparts + 2 * i)) & 3);
        }
    }
    int planesel = 0;
    int below = 128 - wbits - high;
    if (D) { planesel = (fwd[below - 2]) | (fwd[below - 1] << 1); below -= 2; }
    int nvals = 0; for (int i = 0; i < nparts; i++) nvals += ((cem[i] >> 2) + 1) * 2;
    if (nvals > 18) { error_block(bw, bh, rgba); return; }
    int avail = below - cstart, cq = -1;
    for (int q = 20; q >= 4; q--) if (ise_bits(nvals, q) <= avail) { cq = q; break; }
    if (cq < 0) { error_block(bw, bh, rgba); return; }
    uint8_t cv[18]; ise_decode(fwd, cstart, nvals, cq, cv);
    uint8_t wv[128]; ise_decode(rev, 0, nweights, wq, wv);
    int e0[4][4], e1[4][4], k = 0;
    for (int p = 0; p < nparts; p++) {
        int n = ((cem[p] >> 2) + 1) * 2, v[8];
        for (int i = 0; i < n; i++) v[i] = unq_color(cv[k + i], cq);
        k += n;
        if (!endpoints(cem[p] , v, e0[p], e1[p])) { error_block(bw, bh, rgba); return; }
    }
    int w[2][144];                                                           // the weight grid, unquantised (index = x + y * xw)
    for (int i = 0; i < (int)(xw * yw); i++) { w[0][i] = unq_weight(wv[D ? 2 * i : i], wq); if (D) w[1][i] = unq_weight(wv[2 * i + 1], wq); }
    int ds = (1024 + bw / 2) / (bw - 1 ? bw - 1 : 1), dt = (1024 + bh / 2) / (bh - 1 ? bh - 1 : 1), small = bw * bh < 31;
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
        int gs = ((ds * x) * ((int)xw - 1) + 32) >> 6, gt = ((dt * y) * ((int)yw - 1) + 32) >> 6;
        int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
        int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
        int wt[2];
        for (int pl = 0; pl <= (int)D; pl++) {
            int v0 = js + jt * (int)xw, n = (int)(xw * yw);
            int p00 = v0 < n ? w[pl][v0] : 0, p01 = v0 + 1 < n ? w[pl][v0 + 1] : 0, p10 = v0 + (int)xw < n ? w[pl][v0 + (int)xw] : 0, p11 = v0 + (int)xw + 1 < n ? w[pl][v0 + (int)xw + 1] : 0;
            wt[pl] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
        }
        int part = nparts > 1 ? partition_of(seed, x, y, nparts, small) : 0;
        for (int c = 0; c < 4; c++) {
            int wgt = D && c == planesel ? wt[1] : wt[0];
            int c0 = (e0[part][c] << 8) | e0[part][c], c1 = (e1[part][c] << 8) | e1[part][c];
            rgba[4 * (y * bw + x) + c] = (uint8_t)(((c0 * (64 - wgt) + c1 * wgt + 32) >> 6) >> 8);
        }
    }
}

void astc_decode_image(const uint8_t *blocks, int w, int h, int bw, int bh, uint8_t *rgba) {
    int nbx = (w + bw - 1) / bw, nby = (h + bh - 1) / bh;
    uint8_t tile[12 * 12 * 4];
    for (int by = 0; by < nby; by++) for (int bx = 0; bx < nbx; bx++) {
        astc_decode_block(blocks + 16 * (size_t)(by * nbx + bx), bw, bh, tile);
        for (int y = 0; y < bh && by * bh + y < h; y++)
            for (int x = 0; x < bw && bx * bw + x < w; x++) memcpy(rgba + 4 * ((size_t)(by * bh + y) * w + bx * bw + x), tile + 4 * (y * bw + x), 4);
    }
}
