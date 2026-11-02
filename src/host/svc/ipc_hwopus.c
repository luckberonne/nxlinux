// hwopus: the hardware Opus decoder service (nn::audio::codec's OpusDecoder). The console decodes on a DSP; here libopus does it. IHardwareOpusDecoderManager opens a decoder
// (single stream or multistream) and says how big a work buffer the client must give it (the client's memory is not used: the decoder lives in the host); IHardwareOpusDecoder
// decodes one packet a call: the packet in its buffer starts with 8 bytes (the payload size and the final range, both big endian), the PCM comes back as interleaved s16.
// Command layouts: switchbrew "Audio services" (hwopus). Super Smash Bros. Ultimate decodes its music with it (SDK 8.2.1: DecodeInterleaved, command 6).
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NX_HAVE_OPUS
#include <opus/opus.h>
#include <opus/opus_multistream.h>
#endif

#define R_NOT_IMPL 0xF201u
#define R_INVALID 0x1A0Cu       // an Opus error, as the service reports it
#define OUT32(v) (out->out[out->nout++] = (uint32_t)(v))
#define OUT64(v) do { uint64_t _v = (v); OUT32(_v); OUT32(_v >> 32); } while (0)

#ifdef NX_HAVE_OPUS
typedef struct { OpusDecoder *dec; OpusMSDecoder *ms; int rate, channels; } hwopus_dec_t;

static void dec_fin(void *p) {
    hwopus_dec_t *d = p;
    if (d->dec) opus_decoder_destroy(d->dec);
    if (d->ms) opus_multistream_decoder_destroy(d->ms);
    free(d);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// One packet: in->send[0] holds the 8-byte header and the Opus data, the output buffer gets the PCM. Returns the Result; consumed (header + payload) and samples go to the reply.
static uint32_t decode(const ipc_in_t *in, ipc_out_t *out, int reset, int perf) {
    hwopus_dec_t *d = in->data;
    if (!d || !in->nsend || in->send[0].size < 8) return R_INVALID;
    const uint8_t *pk = (const uint8_t *)(uintptr_t)in->send[0].addr;
    uint32_t size = be32(pk);
    if (size > in->send[0].size - 8) return R_INVALID;
    uint64_t oaddr = in->nrecv ? in->recv[0].addr : 0; unsigned osize = in->nrecv ? in->recv[0].size : 0;
    if (!osize && in->nrecv >= 2 && !in->recv[1].size && in->nargs >= 3) {      // the output as a pointer entry, read one word off by the list parse (see ipc_audout.c)
        oaddr = (uint64_t)in->args[in->nargs - 1] | ((uint64_t)(in->recv[1].addr & 0xffff) << 32); osize = (unsigned)(in->recv[1].addr >> 16);
    }
    if (!osize) return R_INVALID;
    int frame = (int)(osize / (2u * (unsigned)d->channels)), n;
    if (reset) { if (d->dec) opus_decoder_ctl(d->dec, OPUS_RESET_STATE); else opus_multistream_decoder_ctl(d->ms, OPUS_RESET_STATE); }
    if (d->dec) n = opus_decode(d->dec, size ? pk + 8 : NULL, (opus_int32)size, (opus_int16 *)(uintptr_t)oaddr, frame, 0);
    else n = opus_multistream_decode(d->ms, size ? pk + 8 : NULL, (opus_int32)size, (opus_int16 *)(uintptr_t)oaddr, frame, 0);
    if (n < 0) { static int once; if (!once++) fprintf(stderr, "[RED] hwopus: the packet (%u bytes) does not decode (%s)\n", size, opus_strerror(n)); return R_INVALID; }
    OUT32(8 + size); OUT32(n);
    if (perf) OUT64(0);
    return 0;
}

static uint32_t decoder(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: return decode(in, out, 0, 0);                          // DecodeInterleavedOld
    case 2: return decode(in, out, 0, 0);                          // DecodeInterleavedForMultiStreamOld
    case 4: case 5: return decode(in, out, 0, 1);                  // ...WithPerfOld
    case 6: case 7: return decode(in, out, in->nargs && (in->args[0] & 1), 1);     // DecodeInterleaved(ForMultiStream)(bool reset) -> consumed, samples, performance
    case 1: case 3: return 0;                                      // SetContext(ForMultiStream): nothing to keep
    default: fprintf(stderr, "[RED] ipc: hwopus IHardwareOpusDecoder command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

static uint32_t work_size(int channels) { int n = opus_decoder_get_size(channels); return (uint32_t)((n > 0 ? n : 0x4000) + 0x1000); }

static uint32_t manager(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: case 4: {                                              // OpenHardwareOpusDecoder(Ex)(OpusParameters {sample rate, channels}, work buffer size; transfer memory)
        int rate = (int)in->args[0], ch = (int)in->args[1], err = 0;
        hwopus_dec_t *d = calloc(1, sizeof *d); if (!d) return 0x105u;
        d->rate = rate; d->channels = ch; d->dec = opus_decoder_create(rate, ch, &err);
        if (!d->dec) { free(d); fprintf(stderr, "[RED] hwopus: no decoder for %d Hz, %d channels (%s)\n", rate, ch, opus_strerror(err)); return R_INVALID; }
        out->move[out->nmove++] = ipc_new_session_owned("hwopus.d", decoder, d, dec_fin);
        return 0;
    }
    case 1: case 5: OUT32(work_size((int)in->args[1])); return 0;  // GetWorkBufferSize(Ex)(OpusParameters)
    case 2: case 6: {                                              // OpenHardwareOpusDecoderForMultiStream(Ex)(parameters in a buffer, work buffer size; transfer memory)
        if (!in->nsend || in->send[0].size < 16) return R_INVALID;
        const int32_t *pm = (const int32_t *)(uintptr_t)in->send[0].addr;     // sample rate, channels, total streams, stereo streams, then a mapping of 0x100 bytes
        const uint8_t *map = (const uint8_t *)(pm + 4); int err = 0;
        hwopus_dec_t *d = calloc(1, sizeof *d); if (!d) return 0x105u;
        d->rate = pm[0]; d->channels = pm[1]; d->ms = opus_multistream_decoder_create(pm[0], pm[1], pm[2], pm[3], map, &err);
        if (!d->ms) { free(d); fprintf(stderr, "[RED] hwopus: no multistream decoder (%s)\n", opus_strerror(err)); return R_INVALID; }
        out->move[out->nmove++] = ipc_new_session_owned("hwopus.d", decoder, d, dec_fin);
        return 0;
    }
    case 3: case 7: {                                              // GetWorkBufferSizeForMultiStream(Ex)
        const int32_t *pm = in->nsend ? (const int32_t *)(uintptr_t)in->send[0].addr : NULL;
        OUT32(work_size(pm ? pm[1] : 2) + 0x1000); return 0;
    }
    default: fprintf(stderr, "[RED] ipc: hwopus command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

__attribute__((constructor)) static void register_hwopus(void) { ipc_register_service("hwopus", manager); }
#endif
