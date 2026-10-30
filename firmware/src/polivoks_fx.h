/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* BBD echo for the Polivoks bus: a bucket-brigade delay, dark and a little wobbly, that saturates as it repeats.
 * Runs at half the output rate (22.05 kHz), as the real chips' clocked delays are dark anyway, which halves
 * memory and CPU. Audio interrupt only; set the parameters from the same context. */
#ifndef FM1_POLIVOKS_FX_H
#define FM1_POLIVOKS_FX_H
#include <stdint.h>

#define POLIVOKS_FX_PARAMS 4
enum { FX_DLY_TIME, FX_DLY_FEED, FX_DLY_TONE, FX_DLY_MIX };
extern const uint8_t polivoks_fx_default[POLIVOKS_FX_PARAMS];

void polivoks_fx_reset(void);
void polivoks_fx_set(unsigned parameter, unsigned value);
/* Reads 128 mono samples (Q23), adds the echo to left/right (Q23, 128 samples each). */
void polivoks_fx_process(const int32_t *in, int32_t *left, int32_t *right);
#endif
