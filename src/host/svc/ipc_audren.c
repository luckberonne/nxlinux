// audren:u, the audio renderer service. The title's nn::audio library builds a block of voice, memory pool and sink descriptions every 5 ms frame and sends it with RequestUpdate; the
// service answers with the new state of each memory pool and how many wave buffers each voice has played. Formats: libnx's audren.h (revisions 1 to 4) and the Nintendo SDK's client.
// The host renders the sound (svc/audren_mix.c): while the renderer is started a clock thread renders a frame every 5 ms, queues it on the SDL3 audio device and fires the
// frame event. NX_AUDIO=0 keeps the device closed (the frames are still rendered, so streaming advances); NX_AUDIO_WAV=file also writes them as a 16-bit stereo WAV.
#include "ipc.h"
#include "kernel.h"
#include "audren_mix.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef NX_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

#define R_NOT_IMPL 0xF201u
#define OUT32(v) (out->out[out->nout++] = (uint32_t)(v))
#define OUT64(v) do { uint64_t _v = (v); OUT32(_v); OUT32(_v >> 32); } while (0)

typedef struct {
    uint32_t p[13];                           // AudioRendererParameter: sample rate, sample count, mix buffers, submixes, voices, sinks, effects, ..., revision at [12]
    uint32_t event;                           // the readable end of the frame event
    int started, stop;
    pthread_t ticker; int ticking;
    pthread_mutex_t lock;                     // the mixer, between RequestUpdate and the clock thread
    ar_mixer_t *mix;
    FILE *wav; uint32_t wav_bytes;
#ifdef NX_HAVE_SDL3
    SDL_AudioStream *stream;
#endif
} renderer_t;

static void wav_header(renderer_t *r) {       // RIFF header for what has been written so far
    uint32_t rate = r->p[0], h[11] = { 0x46464952, 36 + r->wav_bytes, 0x45564157, 0x20746d66, 16, 0x00020001, rate, rate * 4, 0x00100004, 0x61746164, r->wav_bytes };
    long at = ftell(r->wav); fseek(r->wav, 0, SEEK_SET); fwrite(h, 4, 11, r->wav); fseek(r->wav, at, SEEK_SET); fflush(r->wav);
}

static void output_open(renderer_t *r) {
    const char *wav = getenv("NX_AUDIO_WAV");
    if (wav && *wav && (r->wav = fopen(wav, "wb"))) wav_header(r);
    const char *on = getenv("NX_AUDIO");
    if (on && *on == '0') return;
#ifdef NX_HAVE_SDL3
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");          // when audio starts SDL first (no window), SIGINT and SIGTERM must still end the process
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "[RED] audren: SDL audio could not start (%s): no sound\n", SDL_GetError()); return; }
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, (int)r->p[0] };
    r->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!r->stream) { fprintf(stderr, "[RED] audren: no audio device (%s): no sound\n", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(r->stream);
#else
    fprintf(stderr, "[RED] audren: built without SDL3: no sound\n");
#endif
}

static void output_write(renderer_t *r, const float *f, uint32_t n) {   // n stereo frames
#ifdef NX_HAVE_SDL3
    // the device's clock and the host's drift apart: more than 100 ms queued means the device is behind, and the frame is dropped
    if (r->stream && SDL_GetAudioStreamQueued(r->stream) < (int)(r->p[0] / 10 * 8)) SDL_PutAudioStreamData(r->stream, f, (int)(n * 8));
#endif
    if (r->wav) {
        int16_t s[2048];
        for (uint32_t k = 0; k < 2 * n && k < 2048; k++) s[k] = (int16_t)(f[k] * 32767.0f);
        fwrite(s, 2, 2 * n, r->wav); r->wav_bytes += 4 * n;
        if (r->wav_bytes % (r->p[0] * 4) < 4 * n) wav_header(r);   // about once a second
    }
}

static void *tick(void *arg) {                // the audio frame clock: sample count / sample rate, 5 ms
    renderer_t *r = arg;
    uint32_t n = r->p[1] && r->p[1] <= 1024 ? r->p[1] : 240;
    long period = (long)(1000000000ull * n / (r->p[0] ? r->p[0] : 48000));
    float frame[2 * 1024];
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    while (!r->stop) {
        t.tv_nsec += period; if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - t.tv_sec) * 1000000000l + (now.tv_nsec - t.tv_nsec) > 100000000l) t = now;   // more than 100 ms late (suspended): start over
        if (!r->started) continue;
        pthread_mutex_lock(&r->lock); ar_mixer_render(r->mix, frame); pthread_mutex_unlock(&r->lock);
        output_write(r, frame, n);
        kernel_event_signal(r->event);
    }
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
        pthread_mutex_lock(&r->lock);
        uint32_t rc = ar_mixer_update(r->mix, (const uint8_t *)(uintptr_t)in->send[0].addr, in->send[0].size, (uint8_t *)(uintptr_t)in->recv[0].addr, in->recv[0].size);
        pthread_mutex_unlock(&r->lock);
        return rc;
    }
    case 5: r->started = 1; return 0;                       // Start
    case 6: r->started = 0; return 0;                       // Stop
    case 7: out->copy[out->ncopy++] = r->event; return 0;   // QuerySystemEvent → the frame event
    case 8: return 0;                                       // SetRenderingTimeLimit
    case 9: OUT32(100); return 0;                           // GetRenderingTimeLimit
    case 11: return 0;                                      // ExecuteAudioRendererRendering
    default: fprintf(stderr, "[RED] ipc: audren IAudioRenderer command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
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
    default: fprintf(stderr, "[RED] ipc: audren IAudioDevice command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

static uint32_t manager(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: case 3: {                                       // OpenAudioRenderer(parameters, work buffer size, aruid; transfer memory and process handles) → IAudioRenderer
        renderer_t *r = calloc(1, sizeof *r);
        memcpy(r->p, in->args, sizeof r->p);
        r->mix = ar_mixer_new(r->p);
        pthread_mutex_init(&r->lock, NULL);
        r->event = kernel_event_new(0);
        output_open(r);
        r->ticking = !pthread_create(&r->ticker, NULL, tick, r); if (r->ticking) pthread_detach(r->ticker);
        out->move[out->nmove++] = ipc_new_session_data("IAudioRenderer", renderer, r);
        return 0;
    }
    case 1: {                                               // GetWorkBufferSize(parameters) → bytes: room for voices, effects, sinks and the mixer, page aligned
        uint64_t sz = 0x100000 + (uint64_t)in->args[4] * 0x2000 + (uint64_t)in->args[6] * 0x10000 + (uint64_t)in->args[5] * 0x1000 + (uint64_t)in->args[2] * 0x400;
        OUT64((sz + 0xfff) & ~0xfffull); return 0;
    }
    case 2: case 4: out->move[out->nmove++] = ipc_new_session("IAudioDevice", device); return 0;   // GetAudioDeviceService(aruid) / WithRevisionInfo
    default: fprintf(stderr, "[RED] ipc: audren:u command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

__attribute__((constructor)) static void register_audren(void) {
    ipc_register_service("audren:u", manager);
    ipc_register_service("audren:a", manager);
}
