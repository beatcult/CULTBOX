/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* BBD echo for the Polivoks bus: a bucket-brigade delay, dark and a little wobbly, that saturates as it repeats.
 * Runs at half the output rate (22.05 kHz), as the real chips' clocked delays are dark anyway, which halves
 * memory and CPU. Audio interrupt only; set the parameters from the same context. */
#ifndef FM1_POLIVOKS_FX_H
#define FM1_POLIVOKS_FX_H
#include <stdint.h>

#define POLIVOKS_FX_PARAMS 4
#define POLIVOKS_FX_UNITS 2              /* two echoes: 0 for the drums and PLVKS, 1 for DUBMARINE */
#define POLIVOKS_FX_NOTES 12             /* the note values of a tempo-synced delay */
enum { FX_DLY_TIME, FX_DLY_FEED, FX_DLY_TONE, FX_DLY_MIX };
extern const uint8_t polivoks_fx_default[POLIVOKS_FX_PARAMS];

void polivoks_fx_reset(void);
void polivoks_fx_set(unsigned unit, unsigned parameter, unsigned value);
/* Tempo sync: on, the delay is a note value (0..11: 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2) of the beat; a delay too long
 * for the buffer is halved until it fits. polivoks_fx_tempo sets the beat (full-rate samples), once a block. */
void polivoks_fx_sync(unsigned unit, unsigned on, unsigned note);
void polivoks_fx_tempo(float samples_per_beat);
/* Reads 128 mono samples (Q23), adds the echo to left/right (Q23, 128 samples each). */
void polivoks_fx_process(unsigned unit, const int32_t *in, int32_t *left, int32_t *right);
#endif
