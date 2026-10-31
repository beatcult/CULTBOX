/* SPDX-License-Identifier: GPL-3.0-only */
/* Dub siren, after beatcult/dubmarine: a saw oscillator pitch-swept by an LFO (eight shapes), a resonant low-pass
 * with a sweep control, a release ("decay") the echo can ring out of, a hold latch, and a keyboard that plays the
 * pitch with a glide. Mono; float DSP; audio interrupt only. Output: mono Q23, added to out. */
#ifndef FM1_DUBSIREN_H
#define FM1_DUBSIREN_H
#include <stdint.h>
#include "polivoks.h"                           /* fm1_param_t */

#define DUBSIREN_FRAMES 128
enum {
    SIREN_TUNE, SIREN_RATE, SIREN_DEPTH, SIREN_SHAPE,            /* 1 pitch and LFO */
    SIREN_CUTOFF, SIREN_RESO, SIREN_SWEEP, SIREN_SWEEPMODE,      /* 2 filter */
    SIREN_DECAY, SIREN_HOLD, SIREN_GLIDE, SIREN_VOLUME,          /* 3 play */
    SIREN_NPARAMS
};
enum { SIREN_SWEEP_FILTER, SIREN_SWEEP_FILTER_TUNE };
#define DUBSIREN_BANK_BYTES 256
typedef struct dubsiren_bank dubsiren_bank_t;
void dubsiren_init(dubsiren_bank_t *b);
void dubsiren_panic(dubsiren_bank_t *b);
void dubsiren_note(dubsiren_bank_t *b, unsigned note, unsigned velocity);   /* velocity 0: off */
void dubsiren_bend(dubsiren_bank_t *b, int32_t semitones_q16);
void dubsiren_mod(dubsiren_bank_t *b, unsigned value);
void dubsiren_set(dubsiren_bank_t *b, unsigned parameter, unsigned value);
unsigned dubsiren_render(dubsiren_bank_t *b, int32_t *out);                 /* 0 when silent */
const fm1_param_t *dubsiren_param(int parameter);
#endif
