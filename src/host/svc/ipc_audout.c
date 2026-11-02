// audout:u (the audio-out device: nn::audio opens it and feeds raw PCM buffers). The host has no audio device yet, so the device exists but runs at the
// speed of the caller: every buffer that is appended is released again at once, which keeps the title's audio thread going with silence. The stream is
// not played (ipc_audren.c is the mixer that makes sound for a title that renders its own frames).
// Command layouts: switchbrew "Audio services" (audout:u, IAudioOut).
#include "ipc.h"
#include "kernel.h"
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

// The default device: stereo PCM16 at 48 kHz, the same defaults nn::audio asks for (AudioOutParameter with the 0xcafe marker).
#define AU_RATE 48000
#define AU_CHANNELS 2
#define AU_FORMAT 2          // nn::audio::SampleFormat_PcmInt16
#define AU_STOPPED 1         // nn::audio::AudioOutState: 0 started, 1 stopped
static const char g_name[] = "AudioStereoJackOutput";

#define AU_QUEUE 64          // buffers a title may have in flight before the oldest identifier is dropped
typedef struct {
    uint32_t event;          // RegisterBufferEvent's handle (0 until it is registered)
    int started;
    float volume;
    uint64_t id[AU_QUEUE];   // the BufferClientPtr of each appended buffer, waiting to be reported as released
    uint64_t clock_ns;       // when the audio appended so far has been played, at 48 kHz
#ifdef NX_HAVE_SDL3
    SDL_AudioStream *stream; int tried;     // the sound card, opened with the first buffer (NX_AUDIO=0 keeps it closed)
#endif
    int head, count;
} audout_t;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#define AU_LEAD_NS 60000000ull   // how far ahead of the playback a title may queue before Append makes it wait

static void audout_fin(void *p) {
    audout_t *s = p;
#ifdef NX_HAVE_SDL3
    if (s->stream) SDL_DestroyAudioStream(s->stream);
#endif
    free(s);
}

#ifdef NX_HAVE_SDL3
static void audout_open(audout_t *s) {
    s->tried = 1;
    const char *on = getenv("NX_AUDIO"); if (on && *on == '0') return;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "[RED] audout: SDL audio could not start (%s): no sound\n", SDL_GetError()); return; }
    SDL_AudioSpec spec = { SDL_AUDIO_S16, AU_CHANNELS, AU_RATE };
    s->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!s->stream) { fprintf(stderr, "[RED] audout: no audio device (%s): no sound\n", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(s->stream);
}
#endif

static uint32_t audout_session(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    audout_t *s = in->data;
    switch (in->cmd) {
    case 0: OUT32(s->started ? 0 : AU_STOPPED); return 0;         // GetAudioOutState
    case 1: s->started = 1; return 0;                             // Start
    case 2: s->started = 0; return 0;                             // Stop
    case 3: case 7: {                                             // AppendAudioOutBuffer(Auto)(AudioOutBuffer in an input buffer, u64 BufferClientPtr): the Auto one only differs in how the buffer travels (Super Smash Bros. Ultimate uses it)
        uint64_t id = in->nargs >= 2 ? (uint64_t)in->args[0] | ((uint64_t)in->args[1] << 32) : 0;
        if (s->count == AU_QUEUE) { s->head = (s->head + 1) % AU_QUEUE; s->count--; }
        s->id[(s->head + s->count) % AU_QUEUE] = id; s->count++;
        if (s->event) kernel_event_signal(s->event);
        // The buffers are released at once, but the device plays them one after the other: the caller that is further ahead than AU_LEAD_NS waits, with its core free, until the playback has caught up.
        // Released at once and not waiting, a title's audio thread mixes flat out and takes a core of the three the title has (Super Smash Bros. Ultimate's reverb was half of all the guest time).
        uint64_t bytes = in->nsend && in->send[0].size >= 0x20 ? ((const uint64_t *)(uintptr_t)in->send[0].addr)[3] : 3840;   // AudioOutBuffer: next, samples, capacity, data size (+0x18), data offset
#ifdef NX_HAVE_SDL3
        if (in->nsend && in->send[0].size >= 0x28) {         // the samples: the buffer's pointer (+8) and offset (+0x20) say where the PCM16 data of `bytes` bytes is
            const uint64_t *ab = (const uint64_t *)(uintptr_t)in->send[0].addr;
            if (!s->tried) audout_open(s);
            if (s->stream && ab[1] && bytes && bytes <= ab[2] && SDL_GetAudioStreamQueued(s->stream) < (int)(AU_RATE * AU_CHANNELS * 2 / 4)) SDL_PutAudioStreamData(s->stream, (const uint8_t *)(uintptr_t)(ab[1] + ab[4]), (int)bytes);
        }
#endif
        uint64_t now = now_ns(); if (s->clock_ns < now) s->clock_ns = now;
        s->clock_ns += (bytes / (AU_CHANNELS * 2) + 1) * 1000000000ull / AU_RATE;
        if (s->clock_ns > now + AU_LEAD_NS) {
            uint64_t ns = s->clock_ns - now - AU_LEAD_NS; struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
            kernel_core_release(); nanosleep(&ts, NULL); kernel_core_acquire();
        }
        return 0;
    }
    case 4:                                                       // RegisterBufferEvent → the event a released buffer signals
        if (!s->event) s->event = kernel_event_new(0);
        out->copy[out->ncopy++] = s->event; return 0;
    case 5: case 8: {                                             // GetReleasedAudioOutBuffers(Auto): everything appended is already released
        unsigned n = (unsigned)s->count;
        uint64_t oaddr = in->nrecv ? in->recv[0].addr : 0; unsigned osize = in->nrecv ? in->recv[0].size : 0;
        if (in->cmd == 8 && !osize && in->nrecv >= 2 && !in->recv[1].size && in->nargs >= 3) {
            // The Auto form with its output as a pointer entry: the SDK's message has one data word more than the receive-list rule counts, so both entries read as empty;
            // the entry is {address low = the last argument word, size << 16 | address bits 32-47 = the word the list parse took for an address} (Super Smash Bros. Ultimate asks for one buffer a time)
            oaddr = (uint64_t)in->args[2] | ((uint64_t)(in->recv[1].addr & 0xffff) << 32); osize = (unsigned)(in->recv[1].addr >> 16);
        }
        if (osize) {                                              // the identifiers land in the output buffer
            uint64_t *b = (uint64_t *)(uintptr_t)oaddr;
            unsigned room = osize / 8; if (n > room) n = room;
            for (unsigned i = 0; i < n; i++) b[i] = s->id[(s->head + i) % AU_QUEUE];
        } else if (in->nrecv) n = 0;
        OUT32(n);
        s->head = (s->head + (int)n) % AU_QUEUE; s->count -= (int)n;
        return 0;
    }
    case 6: {                                                     // ContainsAudioOutBuffer(u64 BufferClientPtr) → bool
        uint64_t id = in->nargs >= 2 ? (uint64_t)in->args[0] | ((uint64_t)in->args[1] << 32) : 0;
        int found = 0;
        for (int i = 0; i < s->count; i++) if (s->id[(s->head + i) % AU_QUEUE] == id) found = 1;
        OUT32(found); return 0;
    }
    case 9: OUT32((uint32_t)s->count); return 0;                  // GetAudioOutBufferCount
    case 10: OUT64(0); return 0;                                  // GetAudioOutPlayedSampleCount
    case 11: OUT32(0); return 0;                                  // FlushAudioOutBuffers → Pending
    case 12: {                                                    // SetAudioOutVolume(float)
        float v = 1.0f; if (in->nargs >= 1) memcpy(&v, &in->args[0], 4); s->volume = v; return 0;
    }
    case 13: { float v = s->volume; uint32_t w; memcpy(&w, &v, 4); OUT32(w); return 0; }   // GetAudioOutVolume
    default: fprintf(stderr, "[RED] ipc: audout IAudioOut command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

static void put_name(const ipc_in_t *in) {                        // the 0x100-byte device name an output buffer receives
    if (!in->nrecv) return;
    uint8_t *b = (uint8_t *)(uintptr_t)in->recv[0].addr;
    size_t n = in->recv[0].size < 0x100 ? in->recv[0].size : 0x100;
    memset(b, 0, n);
    if (n > sizeof g_name) memcpy(b, g_name, sizeof g_name);
}

static uint32_t audout(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: case 2: {                                             // ListAudioOuts(Auto) → Count, names into the output buffer
        put_name(in);
        OUT32(1); return 0;
    }
    case 1: case 3: {                                             // OpenAudioOut(Auto) → NameOut + AudioOutParameterInternal + IAudioOut
        audout_t *s = calloc(1, sizeof *s);
        if (!s) return 0x105u;
        s->volume = 1.0f;
        out->move[out->nmove++] = ipc_new_session_owned("audout.s", audout_session, s, audout_fin);
        put_name(in);
        OUT32(AU_RATE); OUT32(AU_CHANNELS); OUT32(AU_FORMAT); OUT32(AU_STOPPED);
        return 0;
    }
    default: fprintf(stderr, "[RED] ipc: audout:u command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

__attribute__((constructor)) static void register_audout(void) { ipc_register_service("audout:u", audout); }
