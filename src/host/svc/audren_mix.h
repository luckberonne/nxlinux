// The audio renderer's DSP on the host: the voices, mixes and device sink the title describes in each RequestUpdate, rendered to 48 kHz stereo.
#pragma once
#include <stdint.h>

typedef struct ar_mixer ar_mixer_t;

ar_mixer_t *ar_mixer_new(const uint32_t params[13]);   // AudioRendererParameter: sample rate, sample count, mix buffers, submixes, voices, sinks, effects, ...
void ar_mixer_free(ar_mixer_t *m);
// Applies one RequestUpdate input block and writes the answer (memory pool states, voice play counts) into out. 0, or 0xc01 when a block does not fit.
uint32_t ar_mixer_update(ar_mixer_t *m, const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size);
// Renders one frame: sample count stereo frames, interleaved, at the renderer's sample rate.
void ar_mixer_render(ar_mixer_t *m, float *stereo);
