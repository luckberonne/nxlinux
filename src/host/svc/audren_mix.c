// The audio renderer's DSP, done on the host. Each RequestUpdate carries the title's whole renderer state (libnx audren.h and the SDK client, revision 4 as
// Mario Kart 8 Deluxe sends it): header, behaviour, memory pools, voice channel resources, voices, effects, mixes, sinks, performance. The mixer keeps a
// server copy of every voice (its wave buffer queue, play position and ADPCM history), renders PCM8/PCM16/float/DSP-ADPCM wave buffers with linear
// resampling into the mix buffers its channel resources point at, routes submixes into their destinations through their volume matrices, and maps the
// final mix through the device sink to 5.1, downmixed to stereo. Effects run on their mix before it is routed on, in processing order: auxiliary sends
// (the title's own effect code reads its send ring and fills the return ring), delays; a reverb is not rendered, its input buffers are cleared. The answer tells the title how many samples and wave buffers each voice has played, which is what paces its streaming.
#include "audren_mix.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HDR = 0x40, MEMPOOL_IN = 0x20, MEMPOOL_OUT = 0x10, CHAN_IN = 0x70, VOICE_IN = 0x170, VOICE_OUT = 0x10, EFFECT_IN = 0xc0, EFFECT_OUT = 0x10,
       MIX_IN = 0x930, SINK_IN = 0x140, SINK_OUT = 0x20, PERF_OUT = 0x10, BEHAVIOR_OUT = 0xb0 };
enum { MAX_VOICES = 1024, MAX_CHANS = 1024, MAX_MIXES = 64, MAX_EFFECTS = 64, NBUF = 24, MAX_FRAME = 1024 };
enum { PLAY_STARTED, PLAY_STOPPED, PLAY_PAUSED };
enum { FMT_PCM8 = 1, FMT_PCM16 = 2, FMT_FLOAT = 5, FMT_ADPCM = 6 };

typedef struct { uint64_t addr, size; uint32_t start, end; uint8_t loop, eos; } wavebuf_t;

typedef struct {
    int used, state, fmt, nch, mix;
    uint32_t rate, chan[6];
    float pitch, volume, prev_volume;
    int16_t coef[16];                         // DSP-ADPCM predictor pairs (the voice's extra parameter)
    wavebuf_t wb[4]; uint8_t valid[4];
    int cur;                                  // the wave buffer playing
    uint32_t pos;                             // the next sample in it
    int fresh;                                // pos not yet set from the buffer's start offset
    int32_t hist1, hist2;                     // ADPCM history
    double frac; float prev[6], next[6];      // resampler: between prev and next, frac of the way
    uint64_t played; uint32_t consumed;
} voice_t;

typedef struct { int used, id, dest, nbuf; float volume, mat[NBUF][NBUF]; } mix_t;
typedef struct {
    int used, type, mix, n, order;
    uint8_t in[NBUF], out[NBUF];
    uint64_t send, ret; uint32_t ring;        // aux: the send and return rings, each a 0x80-byte header (the DSP's half at +0x40: read, write offset) and ring int32 samples
    float in_gain, feedback, out_gain, dry_gain, spread, lp_fb, lp_base, lp[6];   // delay
    uint32_t len, cap, at; float *line;       // delay line: len samples per channel, cap allocated
} effect_t;
enum { FX_AUX = 2, FX_DELAY = 3, FX_REVERB = 4, FX_REVERB3D = 5 };

struct ar_mixer {
    uint32_t rate, count, nvoice, nchan, nmix, neffect;
    voice_t voice[MAX_VOICES];
    float chanvol[MAX_CHANS][NBUF];
    mix_t mix[MAX_MIXES];
    effect_t effect[MAX_EFFECTS];
    uint8_t sink_in[6]; int sink_n;
    float buf[MAX_MIXES][NBUF][MAX_FRAME];
    float voicebuf[6][MAX_FRAME];
};

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

ar_mixer_t *ar_mixer_new(const uint32_t p[13]) {
    ar_mixer_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->rate = p[0] ? p[0] : 48000; m->count = p[1] && p[1] <= MAX_FRAME ? p[1] : 240;
    m->nvoice = p[4] < MAX_VOICES ? p[4] : MAX_VOICES; m->nchan = m->nvoice;
    m->nmix = p[3] + 1 < MAX_MIXES ? p[3] + 1 : MAX_MIXES; m->neffect = p[6] < MAX_EFFECTS ? p[6] : MAX_EFFECTS;
    m->sink_n = 2; m->sink_in[0] = 0; m->sink_in[1] = 1;
    return m;
}

void ar_mixer_free(ar_mixer_t *m) {
    if (!m) return;
    for (int i = 0; i < MAX_EFFECTS; i++) free(m->effect[i].line);
    free(m);
}

static float q14(const uint8_t *p) { return (float)(int32_t)rd32(p) / 16384.0f; }

static void effect_update(effect_t *ef, const uint8_t *e) {
    int is_new = e[1];
    ef->type = e[0]; ef->used = e[2] && e[0]; ef->mix = (int)rd32(e + 4); ef->order = (int)rd32(e + 0x18);
    if (ef->type == FX_AUX) {                     // Input[24], Output[24], channel count, sample rate, ring size, send info and storage, return info and storage
        ef->n = (int)rd32(e + 0x50); if (ef->n > NBUF) ef->n = NBUF;
        memcpy(ef->in, e + 0x20, NBUF); memcpy(ef->out, e + 0x38, NBUF);
        ef->ring = rd32(e + 0x58); ef->send = rd64(e + 0x60); ef->ret = rd64(e + 0x70);
        if (rd64(e + 0x68) != ef->send + 0x80 || rd64(e + 0x78) != ef->ret + 0x80) ef->ring = 0;   // not the layout this knows: left alone
    } else if (ef->type >= FX_DELAY && ef->type <= FX_REVERB3D) {   // Input[6], Output[6], channel count max, channel count, then the effect's own fields
        ef->n = *(const uint16_t *)(e + 0x2e); if (ef->n > 6) ef->n = 6;
        memcpy(ef->in, e + 0x20, 6); memcpy(ef->out, e + 0x26, 6);
    } else ef->n = 0;
    if (ef->type != FX_DELAY) return;
    // delay: max and current time (ms), sample rate, then in, feedback, out, dry gains, channel spread and low-pass amount, all Q14
    uint32_t rate = rd32(e + 0x38) >> 14, max_ms = rd32(e + 0x30), ms = rd32(e + 0x34);
    if (!rate) rate = 48000;
    if (ms > max_ms) max_ms = ms;
    ef->in_gain = q14(e + 0x3c); ef->feedback = q14(e + 0x40) * 0.98f; ef->out_gain = q14(e + 0x44); ef->dry_gain = q14(e + 0x48);
    ef->spread = q14(e + 0x4c); ef->lp_fb = 0.95f * q14(e + 0x50); ef->lp_base = 1.0f - ef->lp_fb;
    uint32_t len = (uint32_t)((uint64_t)ms * rate / 1000), cap = (uint32_t)((uint64_t)max_ms * rate / 1000);
    if (len < 1) len = 1; if (cap < len) cap = len;
    if (cap > ef->cap) { free(ef->line); ef->line = calloc((size_t)cap * 6, sizeof(float)); ef->cap = ef->line ? cap : 0; is_new = 1; }
    if (len > ef->cap) len = ef->cap;
    if (len != ef->len) { ef->len = len; is_new = 1; }
    if (is_new && ef->line) { memset(ef->line, 0, (size_t)ef->cap * 6 * sizeof(float)); memset(ef->lp, 0, sizeof ef->lp); ef->at = 0; }
}

static void voice_reset(voice_t *v) {
    memset(v->valid, 0, sizeof v->valid);
    v->cur = 0; v->pos = 0; v->fresh = 1; v->hist1 = v->hist2 = 0; v->frac = 0;
    memset(v->prev, 0, sizeof v->prev); memset(v->next, 0, sizeof v->next);
    v->played = 0; v->consumed = 0;
}

static void voice_flush(voice_t *v, int n) {   // drops n queued wave buffers from the head without playing them
    for (int k = 0; k < n && v->valid[v->cur]; k++) { v->valid[v->cur] = 0; v->consumed++; v->cur = (v->cur + 1) & 3; v->fresh = 1; }
}

static void voice_update(voice_t *v, const uint8_t *in) {
    int used = in[9];
    if (!used) { v->used = 0; return; }
    if (in[8] || !v->used) { voice_reset(v); v->prev_volume = rdf(in + 0x20); }   // is_new
    v->used = 1;
    v->fmt = in[11]; v->rate = rd32(in + 0xc);
    v->nch = (int)rd32(in + 0x18); if (v->nch < 1) v->nch = 1; if (v->nch > 6) v->nch = 6;
    v->pitch = rdf(in + 0x1c); v->volume = rdf(in + 0x20);
    v->mix = (int)rd32(in + 0x58);
    for (int c = 0; c < 6; c++) v->chan[c] = rd32(in + 0x140 + 4 * c);
    uint64_t extra = rd64(in + 0x48), extra_sz = rd64(in + 0x50);
    if (v->fmt == FMT_ADPCM && extra && extra_sz >= sizeof v->coef) memcpy(v->coef, (const void *)(uintptr_t)extra, sizeof v->coef);
    for (int i = 0; i < 4; i++) {                // a wave buffer the client has not sent before joins the queue
        const uint8_t *w = in + 0x60 + 0x38 * i;
        if (w[0x1a]) continue;                   // sent_to_server
        v->wb[i] = (wavebuf_t){ rd64(w), rd64(w + 8), rd32(w + 0x10), rd32(w + 0x14), w[0x18], w[0x19] };
        v->valid[i] = v->wb[i].addr && v->wb[i].end > v->wb[i].start;
    }
    voice_flush(v, in[0x159]);
    int state = in[10];
    if (state == PLAY_STOPPED && v->state != PLAY_STOPPED) {   // stopping flushes the queue, what was just added included, and rewinds
        for (int i = 0; i < 4; i++) if (v->valid[i]) { v->valid[i] = 0; v->consumed++; }
        v->cur = 0; v->pos = 0; v->fresh = 1; v->played = 0; v->hist1 = v->hist2 = 0; v->frac = 0;
        memset(v->prev, 0, sizeof v->prev); memset(v->next, 0, sizeof v->next);
    }
    v->state = state;
}

static float decode(voice_t *v, const wavebuf_t *w, uint32_t n, int ch) {   // sample n of a wave buffer, channel ch
    const uint8_t *a = (const uint8_t *)(uintptr_t)w->addr;
    uint64_t i = (uint64_t)n * v->nch + ch;
    switch (v->fmt) {
    case FMT_PCM16: if ((i + 1) * 2 > w->size) return 0; { int16_t s; memcpy(&s, a + i * 2, 2); return s / 32768.0f; }
    case FMT_PCM8: if (i >= w->size) return 0; return (int8_t)a[i] / 128.0f;
    case FMT_FLOAT: if ((i + 1) * 4 > w->size) return 0; return rdf(a + i * 4);
    case FMT_ADPCM: {                            // mono; frames of 8 bytes: a predictor/scale header and 14 four-bit samples
        uint64_t f = n / 14, k = n % 14;
        if (f * 8 + 1 + k / 2 >= w->size) return 0;
        uint8_t ps = a[f * 8], b = a[f * 8 + 1 + k / 2];
        int nib = (k & 1) ? (b & 0xf) : (b >> 4); if (nib >= 8) nib -= 16;
        int c1 = v->coef[(ps >> 4 & 7) * 2], c2 = v->coef[(ps >> 4 & 7) * 2 + 1];
        int32_t s = ((nib * (1 << (ps & 0xf))) * 2048 + 1024 + c1 * v->hist1 + c2 * v->hist2) >> 11;
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        v->hist2 = v->hist1; v->hist1 = s;
        return s / 32768.0f;
    }
    default: return 0;
    }
}

static int fetch(voice_t *v, float *out) {     // the voice's next source sample, all channels; 0 when its queue is empty
    if (!v->valid[v->cur]) return 0;
    wavebuf_t *w = &v->wb[v->cur];
    if (v->fresh) { v->pos = w->start; v->fresh = 0; }
    for (int c = 0; c < v->nch; c++) out[c] = decode(v, w, v->pos, c);
    v->played++;
    if (++v->pos >= w->end) {
        if (w->loop) v->pos = w->start;
        else {
            v->valid[v->cur] = 0; v->consumed++; v->cur = (v->cur + 1) & 3; v->fresh = 1;
            if (w->eos) v->hist1 = v->hist2 = 0;
        }
    }
    return 1;
}

static mix_t *mix_by_id(ar_mixer_t *m, int id) {
    for (uint32_t i = 0; i < m->nmix; i++) if (m->mix[i].used && m->mix[i].id == id) return &m->mix[i];
    return NULL;
}

uint32_t ar_mixer_update(ar_mixer_t *m, const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size) {
    if (in_size < HDR) return 0xc01u;
    uint32_t behavior = rd32(in + 4), pools = rd32(in + 8), voices = rd32(in + 12), chans = rd32(in + 16), effects = rd32(in + 20), mixes = rd32(in + 24), sinks = rd32(in + 28), perf = rd32(in + 32);
    if ((uint64_t)HDR + behavior + pools + chans + voices + effects + mixes + sinks > in_size) return 0xc01u;
    uint32_t npool = pools / MEMPOOL_IN, nvoice = voices / VOICE_IN, nchan = chans / CHAN_IN, neffect = effects / EFFECT_IN, nsink = sinks / SINK_IN;
    if (nvoice > m->nvoice) nvoice = m->nvoice;
    if (nchan > MAX_CHANS) nchan = MAX_CHANS;
    uint32_t o_pool = npool * MEMPOOL_OUT, o_voice = nvoice * VOICE_OUT, o_effect = neffect * EFFECT_OUT, o_sink = nsink * SINK_OUT, o_perf = perf ? PERF_OUT : 0;
    uint32_t total = HDR + o_pool + o_voice + o_effect + o_sink + o_perf + BEHAVIOR_OUT;
    if (total > out_size) { fprintf(stderr, "[RED] audren: the response (%#x bytes) does not fit the output buffer (%#x)\n", total, out_size); return 0xc01u; }
    const uint8_t *p = in + HDR + behavior;
    memset(out, 0, total);
    uint32_t *oh = (uint32_t *)out;
    oh[0] = rd32(in); oh[1] = BEHAVIOR_OUT; oh[2] = o_pool; oh[3] = o_voice; oh[5] = o_effect; oh[7] = o_sink; oh[8] = o_perf; oh[15] = total;
    // Memory pools: a pool the title asks to attach is attached, one it asks to detach is detached
    for (uint32_t i = 0; i < npool; i++) { uint32_t st = rd32(p + i * MEMPOOL_IN + 0x10); oh[(HDR + i * MEMPOOL_OUT) / 4] = st == 4 ? 5 : st == 2 ? 3 : 0; }
    p += pools;
    for (uint32_t i = 0; i < nchan; i++) {      // channel resources: id, 24 mix volumes, in use; the unused entries are zeros, id 0 included, and change nothing
        const uint8_t *c = p + i * CHAN_IN;
        uint32_t id = rd32(c); if (id >= MAX_CHANS || !c[0x64]) continue;
        for (int b = 0; b < NBUF; b++) m->chanvol[id][b] = rdf(c + 4 + 4 * b);
    }
    p += chans;
    static int trace = -1; if (trace < 0) trace = getenv("NX_AUDREN_TRACE") != NULL;
    for (uint32_t i = 0; i < nvoice; i++) {
        const uint8_t *in = p + i * VOICE_IN; voice_t *v = &m->voice[i];
        voice_update(v, in);
        if (!trace || !in[8] || !in[9]) continue;    // a new voice: what it plays and where it goes
        fprintf(stderr, "[audren] voice %u: format %d, %u Hz, %d channels, pitch %.3f, volume %.3f, state %d, mix %d:", rd32(in), v->fmt, v->rate, v->nch, v->pitch, v->volume, in[10], v->mix);
        for (int c = 0; c < v->nch; c++) {
            fprintf(stderr, " [ch%d res %u", c, v->chan[c]);
            if (v->chan[c] < MAX_CHANS) for (int b = 0; b < NBUF; b++) if (m->chanvol[v->chan[c]][b] != 0.0f) fprintf(stderr, " b%d=%.3f", b, m->chanvol[v->chan[c]][b]);
            fprintf(stderr, "]");
        }
        fprintf(stderr, "\n");
    }
    p += voices;
    for (uint32_t i = 0; i < neffect && i < MAX_EFFECTS; i++) effect_update(&m->effect[i], p + i * EFFECT_IN);
    m->neffect = neffect < MAX_EFFECTS ? neffect : MAX_EFFECTS;
    p += effects;
    uint32_t nmix = mixes / MIX_IN; const uint8_t *mp = p + (mixes - nmix * MIX_IN);   // splitter data, when there is any, comes first
    if (nmix > MAX_MIXES) nmix = MAX_MIXES;
    m->nmix = nmix;
    for (uint32_t i = 0; i < nmix; i++) {
        const uint8_t *x = mp + i * MIX_IN; mix_t *mx = &m->mix[i];
        mx->volume = rdf(x); mx->nbuf = (int)rd32(x + 8); if (mx->nbuf > NBUF) mx->nbuf = NBUF;
        mx->used = x[0xc]; mx->id = (int)rd32(x + 0x10); mx->dest = (int)rd32(x + 0x924);
        memcpy(mx->mat, x + 0x24, sizeof mx->mat);
    }
    p += mixes;
    for (uint32_t i = 0; i < nsink; i++) {      // the device sink: which final mix buffer feeds each device channel
        const uint8_t *s = p + i * SINK_IN;
        if (s[0] != 1 || !s[1]) continue;
        uint32_t n = rd32(s + 0x120); if (n > 6) n = 6;
        if (n) { m->sink_n = (int)n; memcpy(m->sink_in, s + 0x124, n); }
    }
    // Voices: how far each has played
    uint8_t *ov = out + HDR + o_pool;
    for (uint32_t i = 0; i < nvoice; i++) { memcpy(ov + i * VOICE_OUT, &m->voice[i].played, 8); memcpy(ov + i * VOICE_OUT + 8, &m->voice[i].consumed, 4); }
    return 0;
}

// Auxiliary: each channel's frame goes into the send ring as int32 (PCM16 scale) and the channel is replaced by the same span of the return ring, which the
// title fills; channel i sits i frames past the shared offset, and after the last channel both offsets move on by channels * frame.
static void aux_run(effect_t *ef, float (*b)[MAX_FRAME], uint32_t n) {
    if (!ef->ring || !ef->send || !ef->ret) return;
    uint32_t *sd = (uint32_t *)(uintptr_t)(ef->send + 0x40), *rd = (uint32_t *)(uintptr_t)(ef->ret + 0x40);
    int32_t *ss = (int32_t *)(uintptr_t)(ef->send + 0x80), *rs = (int32_t *)(uintptr_t)(ef->ret + 0x80);
    for (int c = 0; c < ef->n; c++) {
        if (ef->in[c] >= NBUF || ef->out[c] >= NBUF) continue;
        uint32_t w = (sd[1] + (uint32_t)c * n) % ef->ring, r = (rd[0] + (uint32_t)c * n) % ef->ring;
        const float *src = b[ef->in[c]];
        for (uint32_t k = 0; k < n; k++) { float v = src[k] * 32768.0f; ss[(w + k) % ef->ring] = v > 2147483520.0f ? INT32_MAX : v < -2147483520.0f ? INT32_MIN : (int32_t)v; }
        float *dst = b[ef->out[c]];
        for (uint32_t k = 0; k < n; k++) dst[k] = (float)rs[(r + k) % ef->ring] / 32768.0f;
    }
    sd[1] = (sd[1] + (uint32_t)ef->n * n) % ef->ring;
    rd[0] = (rd[0] + (uint32_t)ef->n * n) % ef->ring;
}

// Delay: per channel a line of the delay time; what goes in is the input times its gain plus the low-passed feedback (its own channel, and with channel
// spread the neighbouring ones); what comes out is the dry input plus the line's output, each with its gain.
static void delay_run(effect_t *ef, float (*b)[MAX_FRAME], uint32_t n) {
    if (!ef->line || !ef->len) return;
    static const int nb[6][2] = { {1, 2}, {0, 3}, {0, 3}, {1, 2}, {-1, -1}, {-1, -1} };   // 4 channels: front left/right with their rear neighbours
    int nc = ef->n;
    float direct = ef->feedback * (1.0f - ef->spread), cross = ef->feedback * ef->spread * 0.5f;
    for (int c = 0; c < nc; c++) if (ef->in[c] >= NBUF || ef->out[c] >= NBUF) return;
    for (uint32_t k = 0; k < n; k++) {
        float x[6], d[6];
        for (int c = 0; c < nc; c++) { x[c] = b[ef->in[c]][k]; d[c] = ef->line[(size_t)c * ef->cap + ef->at]; }
        for (int c = 0; c < nc; c++) {
            float fb = direct * d[c];
            if (nc == 2) fb += cross * d[c ^ 1];
            else if (nc == 4) fb += cross * (d[nb[c][0]] + d[nb[c][1]]);
            float t = x[c] * ef->in_gain + fb;
            ef->lp[c] = t * ef->lp_base + ef->lp[c] * ef->lp_fb;
            ef->line[(size_t)c * ef->cap + ef->at] = ef->lp[c];
            b[ef->out[c]][k] = x[c] * ef->dry_gain + d[c] * ef->out_gain;
        }
        if (++ef->at >= ef->len) ef->at = 0;
    }
}

static void run_effects(ar_mixer_t *m, int mi, uint32_t n) {   // the effects on one mix, by processing order (ties in table order)
    float (*b)[MAX_FRAME] = m->buf[mi];
    int idx[MAX_EFFECTS], cnt = 0;
    for (uint32_t e = 0; e < m->neffect; e++) {
        if (!m->effect[e].used || m->effect[e].mix != m->mix[mi].id) continue;
        int j = cnt++;
        while (j > 0 && m->effect[idx[j - 1]].order > m->effect[e].order) { idx[j] = idx[j - 1]; j--; }
        idx[j] = (int)e;
    }
    for (int i = 0; i < cnt; i++) {
        effect_t *f = &m->effect[idx[i]];
        if (f->type == FX_AUX) aux_run(f, b, n);
        else if (f->type == FX_DELAY) delay_run(f, b, n);
        else if (f->type == FX_REVERB || f->type == FX_REVERB3D) for (int c = 0; c < f->n; c++) { if (f->in[c] < NBUF) memset(b[f->in[c]], 0, n * sizeof(float)); }
    }
}

static int depth(ar_mixer_t *m, const mix_t *x) {   // hops to the final mix
    int d = 0;
    while (x && x->id != 0 && d < MAX_MIXES) { x = mix_by_id(m, x->dest); d++; }
    return d;
}

void ar_mixer_render(ar_mixer_t *m, float *stereo) {
    uint32_t n = m->count;
    for (uint32_t i = 0; i < m->nmix; i++) for (int b = 0; b < NBUF; b++) memset(m->buf[i][b], 0, n * sizeof(float));
    // Voices into the mix buffers their channel resources name
    for (uint32_t i = 0; i < m->nvoice; i++) {
        voice_t *v = &m->voice[i];
        if (!v->used || v->state != PLAY_STARTED || !v->valid[v->cur]) { if (v->used) v->prev_volume = v->volume; continue; }
        mix_t *dst = mix_by_id(m, v->mix);
        if (!dst) continue;
        float (*db)[MAX_FRAME] = m->buf[dst - m->mix];
        double step = (double)v->rate * (v->pitch > 0 ? v->pitch : 1.0f) / m->rate;
        float (*s)[MAX_FRAME] = m->voicebuf;    // the voice's channels at the output rate, then added into each mix buffer with a gain
        for (uint32_t k = 0; k < n; k++) {
            while (v->frac >= 1.0) {
                memcpy(v->prev, v->next, sizeof v->prev);
                if (!fetch(v, v->next)) memset(v->next, 0, sizeof v->next);
                v->frac -= 1.0;
            }
            float t = (float)v->frac, vol = v->prev_volume + (v->volume - v->prev_volume) * (float)k / (float)n;
            for (int c = 0; c < v->nch; c++) s[c][k] = (v->prev[c] + (v->next[c] - v->prev[c]) * t) * vol;
            v->frac += step;
        }
        int nb = dst->nbuf ? dst->nbuf : NBUF;
        for (int c = 0; c < v->nch; c++) {
            uint32_t id = v->chan[c]; if (id >= MAX_CHANS) continue;
            const float *g = m->chanvol[id];
            for (int b = 0; b < nb; b++) {
                if (g[b] == 0.0f) continue;
                float gb = g[b], *d = db[b]; const float *src = s[c];
                for (uint32_t k = 0; k < n; k++) d[k] += src[k] * gb;
            }
        }
        v->prev_volume = v->volume;
    }
    // Submixes into their destinations, deepest first
    for (int d = MAX_MIXES; d > 0; d--) {
        for (uint32_t i = 0; i < m->nmix; i++) {
            mix_t *x = &m->mix[i];
            if (!x->used || x->id == 0 || depth(m, x) != d) continue;
            run_effects(m, (int)i, n);
            mix_t *to = mix_by_id(m, x->dest);
            if (!to) continue;
            float (*sb)[MAX_FRAME] = m->buf[i], (*tb)[MAX_FRAME] = m->buf[to - m->mix];
            for (int a = 0; a < x->nbuf; a++) for (int b = 0; b < to->nbuf; b++) {
                float g = x->mat[a][b] * x->volume;
                if (g != 0.0f) for (uint32_t k = 0; k < n; k++) tb[b][k] += sb[a][k] * g;
            }
        }
    }
    // The final mix through the sink: 5.1 (front left, front right, centre, LFE, rear left, rear right) or stereo, to stereo
    mix_t *fin = mix_by_id(m, 0);
    if (!fin) { memset(stereo, 0, n * 2 * sizeof(float)); return; }
    run_effects(m, (int)(fin - m->mix), n);
    float (*fb)[MAX_FRAME] = m->buf[fin - m->mix];
    float vol = fin->volume;
    for (uint32_t k = 0; k < n; k++) {
        float ch[6] = {0};
        for (int j = 0; j < m->sink_n; j++) if (m->sink_in[j] < NBUF) ch[j] = fb[m->sink_in[j]][k] * vol;
        float l = ch[0], r = ch[1];
        if (m->sink_n == 6) { l += 0.707f * ch[2] + 0.707f * ch[4]; r += 0.707f * ch[2] + 0.707f * ch[5]; }
        stereo[2 * k] = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
        stereo[2 * k + 1] = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
    }
}
