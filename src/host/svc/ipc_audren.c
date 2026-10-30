// audren:u, the audio renderer service. The title's nn::audio library builds a block of voice, memory pool and sink descriptions every 5 ms frame and sends it with RequestUpdate; the
// service answers with the new state of each memory pool and how many wave buffers each voice has played. Formats: libnx's audren.h (revisions 1 to 4) and the Nintendo SDK's client.
// The host renders no sound: memory pools are attached on request, every queued wave buffer counts as played at once, and the frame event fires at 200 Hz while the renderer is started.
#include "ipc.h"
#include "kernel.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define R_NOT_IMPL 0xF201u
#define OUT32(v) (out->out[out->nout++] = (uint32_t)(v))
#define OUT64(v) do { uint64_t _v = (v); OUT32(_v); OUT32(_v >> 32); } while (0)
enum { HDR = 0x40, MEMPOOL_IN = 0x20, MEMPOOL_OUT = 0x10, VOICE_IN = 0x170, VOICE_OUT = 0x10, EFFECT_OUT = 0x10, SINK_OUT = 0x20, PERF_OUT = 0x10, BEHAVIOR_OUT = 0xb0, MAX_VOICES = 1024 };

typedef struct {
    uint32_t p[13];                           // AudioRendererParameter: sample rate, sample count, mix buffers, submixes, voices, sinks, effects, ..., revision at [12]
    uint32_t event;                           // the readable end of the frame event
    int started, stop;
    pthread_t ticker; int ticking;
    uint32_t consumed[MAX_VOICES];            // wave buffers reported played so far, per voice (the client diffs the running total)
    uint64_t played[MAX_VOICES];              // samples
} renderer_t;

static void *tick(void *arg) {                // the audio frame clock: 5 ms
    renderer_t *r = arg;
    while (!r->stop) { struct timespec ts = {0, 5000000}; nanosleep(&ts, NULL); if (r->started) kernel_event_signal(r->event); }
    return NULL;
}

static uint32_t renderer(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    renderer_t *r = in->data;
    switch (in->cmd) {
    case 0: OUT32(r->p[0]); return 0;                       // GetSampleRate
    case 1: OUT32(r->p[1]); return 0;                       // GetSampleCount
    case 2: OUT32(r->p[2]); return 0;                       // GetMixBufferCount
    case 3: OUT32(r->started ? 0 : 1); return 0;            // GetState: 0 started, 1 stopped
    case 4: case 10: {                                      // RequestUpdate / RequestUpdateAuto(input buffer; output and performance buffers)
        if (!in->nsend || !in->nrecv) return 0xc01u;
        const uint32_t *h = (const uint32_t *)(uintptr_t)in->send[0].addr; uint8_t *o = (uint8_t *)(uintptr_t)in->recv[0].addr;
        uint32_t behavior = h[1], pools = h[2], voices_sz = h[3], chans = h[4], effects_sz = h[5], perf = h[8];
        uint32_t npool = pools / MEMPOOL_IN, nvoice = voices_sz % VOICE_IN ? r->p[4] : voices_sz / VOICE_IN;
        uint32_t neffect = r->p[6], nsink = r->p[5];
        if (nvoice > MAX_VOICES) nvoice = MAX_VOICES;
        uint32_t o_pool = npool * MEMPOOL_OUT, o_voice = nvoice * VOICE_OUT, o_effect = neffect * EFFECT_OUT, o_sink = nsink * SINK_OUT, o_perf = perf ? PERF_OUT : 0;
        uint32_t total = HDR + o_pool + o_voice + o_effect + o_sink + o_perf + BEHAVIOR_OUT;
        if (total > in->recv[0].size) { fprintf(stderr, "[RED] audren: the response (%#x bytes) does not fit the output buffer (%#x)\\n", total, in->recv[0].size); return 0xc01u; }
        memset(o, 0, total);
        uint32_t *oh = (uint32_t *)o;
        oh[0] = h[0]; oh[1] = BEHAVIOR_OUT; oh[2] = o_pool; oh[3] = o_voice; oh[5] = o_effect; oh[7] = o_sink; oh[8] = o_perf; oh[15] = total;
        // Memory pools: a pool the title asks to attach is attached, one it asks to detach is detached
        const uint32_t *mp = (const uint32_t *)((const uint8_t *)h + HDR + behavior);
        uint32_t *omp = (uint32_t *)(o + HDR);
        for (uint32_t i = 0; i < npool; i++) { uint32_t st = mp[i * 8 + 4]; omp[i * 4] = st == 4 ? 5 : st == 2 ? 3 : 0; }   // RequestAttach → Attached, RequestDetach → Detached
        // Voices: a started voice has played everything queued on it
        const uint8_t *vin = (const uint8_t *)h + HDR + behavior + pools + chans;
        uint8_t *ov = o + HDR + o_pool;
        for (uint32_t i = 0; i < nvoice; i++) {
            const uint8_t *v = vin + (size_t)i * VOICE_IN;
            if (v[9] && v[10] == 0) { r->consumed[i] += *(const uint32_t *)(v + 0x3c); r->played[i] += (uint64_t)r->p[1] * 1; }   // is_used and Started
            memcpy(ov + i * VOICE_OUT, &r->played[i], 8); memcpy(ov + i * VOICE_OUT + 8, &r->consumed[i], 4);
        }
        (void)effects_sz;
        return 0;
    }
    case 5: r->started = 1; return 0;                       // Start
    case 6: r->started = 0; return 0;                       // Stop
    case 7: out->copy[out->ncopy++] = r->event; return 0;   // QuerySystemEvent → the frame event
    case 8: return 0;                                       // SetRenderingTimeLimit
    case 9: OUT32(100); return 0;                           // GetRenderingTimeLimit
    case 11: return 0;                                      // ExecuteAudioRendererRendering
    default: fprintf(stderr, "[RED] ipc: audren IAudioRenderer command %u is not implemented\\n", in->cmd); return R_NOT_IMPL;
    }
}

static uint32_t device(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    static const char name[] = "AudioStereoJackOutput";
    switch (in->cmd) {
    case 0: case 6: {                                       // ListAudioDeviceName → count; the 0x80-byte names into the output buffer
        if (in->nrecv && in->recv[0].size >= 0x80) { memset((void *)(uintptr_t)in->recv[0].addr, 0, 0x80); memcpy((void *)(uintptr_t)in->recv[0].addr, name, sizeof name); }
        OUT32(1); return 0;
    }
    case 1: case 7: return 0;                               // SetAudioDeviceOutputVolume
    case 2: case 8: { float one = 1.0f; uint32_t w; memcpy(&w, &one, 4); OUT32(w); return 0; }   // GetAudioDeviceOutputVolume
    case 3: case 10:                                        // GetActiveAudioDeviceName
        if (in->nrecv && in->recv[0].size >= 0x80) { memset((void *)(uintptr_t)in->recv[0].addr, 0, 0x80); memcpy((void *)(uintptr_t)in->recv[0].addr, name, sizeof name); }
        return 0;
    case 4: case 11: case 12: out->copy[out->ncopy++] = kernel_event_new(0); return 0;   // QueryAudioDeviceSystemEvent / InputEvent / OutputEvent
    case 5: OUT32(2); return 0;                             // GetActiveChannelCount
    default: fprintf(stderr, "[RED] ipc: audren IAudioDevice command %u is not implemented\\n", in->cmd); return R_NOT_IMPL;
    }
}

static uint32_t manager(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: case 3: {                                       // OpenAudioRenderer(parameters, work buffer size, aruid; transfer memory and process handles) → IAudioRenderer
        renderer_t *r = calloc(1, sizeof *r);
        memcpy(r->p, in->args, sizeof r->p);
        if (r->p[4] > MAX_VOICES) r->p[4] = MAX_VOICES;
        r->event = kernel_event_new(0);
        r->ticking = !pthread_create(&r->ticker, NULL, tick, r); if (r->ticking) pthread_detach(r->ticker);
        out->move[out->nmove++] = ipc_new_session_data("IAudioRenderer", renderer, r);
        return 0;
    }
    case 1: {                                               // GetWorkBufferSize(parameters) → bytes: room for voices, effects, sinks and the mixer, page aligned
        uint64_t sz = 0x100000 + (uint64_t)in->args[4] * 0x2000 + (uint64_t)in->args[6] * 0x10000 + (uint64_t)in->args[5] * 0x1000 + (uint64_t)in->args[2] * 0x400;
        OUT64((sz + 0xfff) & ~0xfffull); return 0;
    }
    case 2: case 4: out->move[out->nmove++] = ipc_new_session("IAudioDevice", device); return 0;   // GetAudioDeviceService(aruid) / WithRevisionInfo
    default: fprintf(stderr, "[RED] ipc: audren:u command %u is not implemented\\n", in->cmd); return R_NOT_IMPL;
    }
}

__attribute__((constructor)) static void register_audren(void) {
    ipc_register_service("audren:u", manager);
    ipc_register_service("audren:a", manager);
}
