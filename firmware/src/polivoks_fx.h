/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Tape / BBD echo and spring reverb for the Polivoks bus.
 *
 * Both run at half the output rate (22.05 kHz): tape echoes, bucket-brigade
 * delays and springs are all dark, and it halves memory and CPU. The echo is a
 * modulated delay line with a darkening, saturating feedback loop. The spring is
 * two recirculating delay loops with an all-pass chain in each, which spreads
 * high frequencies in time the way a real spring does, so repeats "drip".
 *
 * Audio interrupt only, after the parameters have been set from the same context. */
#ifndef FM1_POLIVOKS_FX_H
#define FM1_POLIVOKS_FX_H
#include <stdint.h>

#define POLIVOKS_FX_PARAMS 12
enum {
    FX_DLY_TIME, FX_DLY_FEED, FX_DLY_TONE, FX_DLY_MIX,      /* echo */
    FX_DLY_TYPE, FX_DLY_WOBBLE, FX_DLY_SAT, FX_DLY_WIDTH,   /* echo character: 0 tape, 1 BBD */
    FX_SPR_DECAY, FX_SPR_TONE, FX_SPR_DRIP, FX_SPR_MIX      /* spring */
};
extern const uint8_t polivoks_fx_default[POLIVOKS_FX_PARAMS];

void polivoks_fx_reset(void);
void polivoks_fx_set(unsigned parameter, unsigned value);
/* Reads 128 mono samples (Q23), adds the effect return to left/right (Q23, 128 samples each). */
void polivoks_fx_process(const int32_t *in, int32_t *left, int32_t *right);
#endif
