// The audio renderer's mixer (src/host/svc/audren_mix.c) on hand-built RequestUpdate blocks: a PCM16 stereo voice through a submix into the final mix,
// a DSP-ADPCM voice, play counts, stopping.
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
    if (fails) return 1;
    printf("audren_mix: ok\n");
    return 0;
}
