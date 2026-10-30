// The audio renderer's mixer (src/host/svc/audren_mix.c) on hand-built RequestUpdate blocks: a PCM16 stereo voice through a submix into the final mix,
// a DSP-ADPCM voice, play counts, stopping; a delay and an auxiliary send on the final mix.
#include "audren_mix.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

enum { HDR = 0x40, BEH = 0x10, POOL = 0x20, CHAN = 0x70, VOICE = 0x170, MIX = 0x930, SINK = 0x140, NV = 2, NMIX = 2 };
static uint8_t in[HDR + BEH + POOL + NV * CHAN + NV * VOICE + NMIX * MIX + SINK], out[0x1000];

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void wf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static uint8_t *chan(int i) { return in + HDR + BEH + POOL + i * CHAN; }
static uint8_t *voice(int i) { return in + HDR + BEH + POOL + NV * CHAN + i * VOICE; }
static uint8_t *mix(int i) { return in + HDR + BEH + POOL + NV * CHAN + NV * VOICE + i * MIX; }
static uint8_t *sink(void) { return in + HDR + BEH + POOL + NV * CHAN + NV * VOICE + NMIX * MIX; }
static uint64_t played(int i) { uint64_t v; memcpy(&v, out + HDR + 0x10 + i * 0x10, 8); return v; }
static uint32_t consumed(int i) { uint32_t v; memcpy(&v, out + HDR + 0x10 + i * 0x10 + 8, 4); return v; }

static void wavebuf(uint8_t *v, int slot, const void *addr, uint64_t size, uint32_t end, int sent) {
    uint8_t *w = v + 0x60 + 0x38 * slot;
    w64(w, (uintptr_t)addr); w64(w + 8, size); w32(w + 0x10, 0); w32(w + 0x14, end); w[0x1a] = (uint8_t)sent;
}

static uint32_t update(ar_mixer_t *m) { return ar_mixer_update(m, in, sizeof in, out, sizeof out); }


// One PCM16 voice into final mix buffers 0, 1 and 2; a 10 ms delay (wet only) from buffer 2 to itself, an aux send/return on buffer 1; the sink takes 0 and 2.
static int effects(void) {
    enum { NE = 2, EFFECT = 0xc0 };
    static uint8_t b[HDR + BEH + POOL + CHAN + VOICE + NE * EFFECT + MIX + SINK];
    static uint8_t ring_send[0x80 + 4 * 1440], ring_ret[0x80 + 4 * 1440];
    static int16_t pcm[480];
    const uint32_t params[13] = { 48000, 240, 6, 0, 1, 1, NE };
    ar_mixer_t *m = ar_mixer_new(params);
    uint32_t *h = (uint32_t *)b;
    h[1] = BEH; h[2] = POOL; h[3] = VOICE; h[4] = CHAN; h[5] = NE * EFFECT; h[6] = MIX; h[7] = SINK; h[15] = sizeof b;
    uint8_t *c = b + HDR + BEH + POOL, *v = c + CHAN, *e = v + VOICE, *x = e + NE * EFFECT, *k = x + MIX;
    w32(c, 0); wf(c + 4, 1.0f); wf(c + 8, 1.0f); wf(c + 12, 1.0f); c[0x64] = 1;
    for (int i = 0; i < 480; i++) pcm[i] = 16384;
    v[8] = 1; v[9] = 1; v[11] = 2; w32(v + 0xc, 48000); w32(v + 0x18, 1); wf(v + 0x1c, 1.0f); wf(v + 0x20, 1.0f); w32(v + 0x58, 0); w32(v + 0x140, 0);
    wavebuf(v, 0, pcm, sizeof pcm, 480, 0);
    e[0] = 3; e[1] = 1; e[2] = 1; w32(e + 0x18, 0);                       // delay: buffer 2, one channel, 10 ms at 48 kHz, in 1, feedback 0, out 1, dry 0
    e[0x20] = 2; e[0x26] = 2; e[0x2c] = 1; e[0x2e] = 1; w32(e + 0x30, 10); w32(e + 0x34, 10); w32(e + 0x38, 48000u << 14);
    w32(e + 0x3c, 0x4000); w32(e + 0x44, 0x4000);
    uint8_t *a = e + EFFECT;                                             // aux: buffer 1, one channel, rings of 1440 samples
    a[0] = 2; a[1] = 1; a[2] = 1; w32(a + 4, 0); w32(a + 0x18, 1);
    a[0x20] = 1; a[0x38] = 1; w32(a + 0x50, 1); w32(a + 0x54, 48000); w32(a + 0x58, 1440);
    w64(a + 0x60, (uintptr_t)ring_send); w64(a + 0x68, (uintptr_t)ring_send + 0x80); w64(a + 0x70, (uintptr_t)ring_ret); w64(a + 0x78, (uintptr_t)ring_ret + 0x80);
    for (int i = 0; i < 1440; i++) w32(ring_ret + 0x80 + 4 * i, 1000);
    wf(x, 1.0f); w32(x + 8, 6); x[0xc] = 1; w32(x + 0x924, 0x7fffffff);
    k[0] = 1; k[1] = 1; w32(k + 0x120, 2); k[0x124] = 0; k[0x125] = 2;
    CHECK(ar_mixer_update(m, b, sizeof b, out, sizeof out) == 0);
    float f[3][480];
    for (int i = 0; i < 3; i++) ar_mixer_render(m, f[i]);
    CHECK(fabsf(f[0][2 * 10] - 0.5f) < 1e-3f && fabsf(f[0][2 * 10 + 1]) < 1e-6f);   // dry now, nothing from the delay yet
    CHECK(fabsf(f[1][2 * 10 + 1]) < 1e-6f);
    CHECK(fabsf(f[2][2 * 1 + 1]) < 1e-6f && fabsf(f[2][2 * 10 + 1] - 0.5f) < 1e-3f);   // 480 samples later (the resampler's 2 included)
    int32_t sent; memcpy(&sent, ring_send + 0x80 + 4 * (240 + 10), 4);
    uint32_t woff, roff; memcpy(&woff, ring_send + 0x44, 4); memcpy(&roff, ring_ret + 0x40, 4);
    CHECK(sent == 16384 && woff == 720 && roff == 720);                   // three frames sent and returned
    ar_mixer_free(m);
    return 0;
}

int main(void) {
    const uint32_t params[13] = { 48000, 240, 30, 1, NV, 1, 0 };
    ar_mixer_t *m = ar_mixer_new(params);
    uint32_t *h = (uint32_t *)in;
    h[0] = 0x34564552; h[1] = BEH; h[2] = POOL; h[3] = NV * VOICE; h[4] = NV * CHAN; h[5] = 0; h[6] = NMIX * MIX; h[7] = SINK; h[15] = sizeof in;
    // final mix (id 0): 6 buffers; submix (id 1): 2 buffers, buffer 0 → final 0 at 0.5, buffer 1 → final 1 at 1.0
    wf(mix(0), 1.0f); w32(mix(0) + 8, 6); mix(0)[0xc] = 1; w32(mix(0) + 0x10, 0); w32(mix(0) + 0x924, 0x7fffffff);
    wf(mix(1), 1.0f); w32(mix(1) + 8, 2); mix(1)[0xc] = 1; w32(mix(1) + 0x10, 1); w32(mix(1) + 0x924, 0);
    wf(mix(1) + 0x24 + 0 * 0x60 + 0 * 4, 0.5f); wf(mix(1) + 0x24 + 1 * 0x60 + 1 * 4, 1.0f);
    sink()[0] = 1; sink()[1] = 1; w32(sink() + 0x120, 2); sink()[0x124] = 0; sink()[0x125] = 1;
    // voice 0: PCM16 stereo at 48 kHz into the submix, channel resources 0 (→ buffer 0) and 1 (→ buffer 1), one wave buffer of 480 frames
    static int16_t pcm[480 * 2];
    for (int k = 0; k < 480; k++) { pcm[2 * k] = 16384; pcm[2 * k + 1] = -8192; }
    w32(chan(0), 0); wf(chan(0) + 4, 1.0f); chan(0)[0x64] = 1;
    w32(chan(1), 1); wf(chan(1) + 4 + 4, 1.0f); chan(1)[0x64] = 1;
    uint8_t *v = voice(0);
    v[8] = 1; v[9] = 1; v[10] = 0; v[11] = 2; w32(v + 0xc, 48000); w32(v + 0x18, 2); wf(v + 0x1c, 1.0f); wf(v + 0x20, 1.0f); w32(v + 0x58, 1);
    w32(v + 0x140, 0); w32(v + 0x144, 1);
    wavebuf(v, 0, pcm, sizeof pcm, 480, 0);
    CHECK(update(m) == 0);
    float f[480];
    ar_mixer_render(m, f);
    // the resampler runs two samples behind: frames 0 and 1 are silence, then the constant signal
    CHECK(fabsf(f[0]) < 1e-6f && fabsf(f[2]) < 1e-6f);
    CHECK(fabsf(f[2 * 10] - 0.25f) < 1e-3f && fabsf(f[2 * 10 + 1] + 0.25f) < 1e-3f);
    v[8] = 0; v[0x60 + 0x1a] = 1;                // sent: not queued again
    CHECK(update(m) == 0 && played(0) == 239 && consumed(0) == 0);
    ar_mixer_render(m, f);
    CHECK(update(m) == 0 && played(0) == 479 && consumed(0) == 0);
    ar_mixer_render(m, f);
    CHECK(update(m) == 0 && played(0) == 480 && consumed(0) == 1);   // the queue ran dry
    // stopping flushes the queue and rewinds
    wavebuf(v, 1, pcm, sizeof pcm, 480, 0);
    v[10] = 1;
    CHECK(update(m) == 0 && played(0) == 0 && consumed(0) == 2);
    // voice 1: DSP-ADPCM mono at 32 kHz, one frame: scale 1 << 1, predictor 0 = (2048, 0): each sample is the last plus 2 * nibble
    static int16_t coef[16] = { 2048, 0 };
    static uint8_t adpcm[8] = { 0x01, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 };
    v[9] = 0;
    w32(chan(1), 1); memset(chan(1) + 4, 0, 96); wf(chan(1) + 4, 1.0f);
    uint8_t *a = voice(1);
    a[8] = 1; a[9] = 1; a[10] = 0; a[11] = 6; w32(a + 0xc, 32000); w32(a + 0x18, 1); wf(a + 0x1c, 1.0f); wf(a + 0x20, 1.0f); w32(a + 0x58, 0);
    w64(a + 0x48, (uintptr_t)coef); w64(a + 0x50, sizeof coef); w32(a + 0x140, 1);
    wavebuf(a, 0, adpcm, sizeof adpcm, 14, 0);
    CHECK(update(m) == 0);
    ar_mixer_render(m, f);
    a[8] = 0; a[0x60 + 0x1a] = 1;
    CHECK(update(m) == 0 && played(1) == 14 && consumed(1) == 1);
    // 32 kHz to 48 kHz: output k sits at source k * 2/3 - 2 (the resampler's two samples of delay); source sample j is 2 * (j + 1)
    CHECK(fabsf(f[2 * 3] * 32768.0f - 2) < 0.01f);
    CHECK(fabsf(f[2 * 4] * 32768.0f - 2 * (1 + 2.0f / 3.0f)) < 0.01f);
    ar_mixer_free(m);
    effects();
    if (fails) return 1;
    printf("audren_mix: ok\n");
    return 0;
}
