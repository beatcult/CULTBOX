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

/* One echo with its own state, delay line and sample rate (a plugin makes as many as it has instances; the FM-1 keeps two, below). */
typedef struct {
    uint8_t pot[POLIVOKS_FX_PARAMS];
    uint8_t sync, note;                 /* tempo sync: the delay is a note value of the beat */
    float beat;                         /* samples (half rate) a beat lasts */
    unsigned wr, n, mask;               /* write position; the delay line's length (a power of two) and its mask */
    int16_t *buf;                       /* the delay line, n samples, the caller's */
    float half, rate;                   /* the echo's sample rate (half the host's), and that over the FM-1's 22050 */
    float len, len_target, fb, lp_a, lp1, lp2, hp_x, hp_y, mix;
    float wow_phase, noise_slow, prev_l, prev_r;
    uint32_t rng;
    unsigned quiet;
} polivoks_fx_t;
unsigned polivoks_fx_buffer_len(float sample_rate);                       /* int16 samples a delay line needs at this host rate */
void polivoks_fx_unit_init(polivoks_fx_t *f, int16_t *buf, unsigned n, float sample_rate, unsigned seed);   /* clears buf; seed varies the wobble */
void polivoks_fx_unit_set(polivoks_fx_t *f, unsigned parameter, unsigned value);
void polivoks_fx_unit_sync(polivoks_fx_t *f, unsigned on, unsigned note);
void polivoks_fx_unit_tempo(polivoks_fx_t *f, float samples_per_beat);   /* the host's beat, in samples at the host's rate */
/* 128 full-rate frames in and out, as polivoks_fx_process; the host's blocks are cut to 128 by the caller. */
unsigned polivoks_fx_unit_process(polivoks_fx_t *f, const int32_t *in, int32_t *left, int32_t *right);

void polivoks_fx_reset(void);
void polivoks_fx_set(unsigned unit, unsigned parameter, unsigned value);
/* Tempo sync: on, the delay is a note value (0..11: 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2) of the beat; a delay too long
 * for the buffer is halved until it fits. polivoks_fx_tempo sets the beat (full-rate samples), once a block. */
void polivoks_fx_sync(unsigned unit, unsigned on, unsigned note);
void polivoks_fx_tempo(float samples_per_beat);
/* Reads 128 mono samples (Q23), adds the echo to left/right (Q23, 128 samples each). */
unsigned polivoks_fx_process(unsigned unit, const int32_t *in, int32_t *left, int32_t *right);   /* 0: it was idle and added nothing */
#endif
