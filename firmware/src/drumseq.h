/* SPDX-License-Identifier: GPL-3.0-only */
/* Drum step sequencer: a pattern of up to 64 steps, ten pads, each step off / hit / accent (2 bits a pad). The clock runs
 * in the audio interrupt, once a block: internal (tempo) or from MIDI clock; hits are placed to the sample, swing delays
 * the odd steps. Output: calls drums_note_at. */
#ifndef FM1_DRUMSEQ_H
#define FM1_DRUMSEQ_H
#include <stdint.h>
#include "drums.h"

#define SEQ_STEPS 64
#define SEQ_STEP_BYTES (SEQ_STEPS * 3)                               /* 3 bytes a step: 10 pads x 2 bits */
enum { SEQ_TEMPO, SEQ_SWING, SEQ_LEN, SEQ_RATE, SEQ_PATTERN, SEQ_SYNC, SEQ_HIT, SEQ_ACC, SEQ_NPARAMS };
/* SEQ_TEMPO 0..200 (40..240 BPM), SEQ_SWING 0..127 (50..75 %), SEQ_LEN 0..3 (16, 32, 48, 64 steps),
 * SEQ_RATE 0..2 (1/8, 1/16, 1/32 a step), SEQ_PATTERN (the slot: not used here), SEQ_SYNC 0 internal, 1 MIDI clock,
 * SEQ_HIT, SEQ_ACC: velocities of a hit and an accent */
enum { SEQ_STOP, SEQ_START, SEQ_CONTINUE, SEQ_CLOCK };
#define SEQ_NONE 255u
typedef struct {
    volatile uint8_t run, pos;           /* running; the step last played (SEQ_NONE: none yet) */
    uint8_t tempo, swing, len, rate, sync, hit, acc;
    float next_in, est;                  /* samples to the next step; one clock's length, measured */
    uint32_t clk, since;
} seq_t;
void seq_init(seq_t *q);
void seq_set(seq_t *q, unsigned parameter, unsigned value);
void seq_command(seq_t *q, unsigned command);                          /* SEQ_STOP, SEQ_START, SEQ_CONTINUE */
void seq_clock_in(seq_t *q, const uint8_t *steps, drums_bank_t *d);   /* one MIDI clock (SEQ_CLOCK); only when running with sync */
void seq_block(seq_t *q, const uint8_t *steps, drums_bank_t *d);   /* the clock moves on by one audio block */
unsigned seq_get(const uint8_t *steps, unsigned step, unsigned pad);                 /* 0 off, 1 hit, 2 accent */
void seq_put(uint8_t *steps, unsigned step, unsigned pad, unsigned state);
#endif
