/* SPDX-License-Identifier: GPL-3.0-only */
/* Step sequencer for the drums and for the PLVKS voice. A pattern is up to 64 steps. The clock runs in the audio interrupt,
 * once a block: internal (tempo) or from MIDI clock. Swing delays the odd steps; each step can also be nudged late.
 *   drums: ten pads, each step off / hit / accent (2 bits a pad), and up to SEQ_NEX edited hits (pitch, velocity, delay, ratchet)
 *   PLVKS: each step has a note, an optional second note, a length and a velocity
 * Hits are placed to the sample (drums_note_ex); PLVKS notes are placed to the audio block, with their note-offs scheduled. */
#ifndef FM1_DRUMSEQ_H
#define FM1_DRUMSEQ_H
#include <stdint.h>
#include "drums.h"
#include "polivoks.h"

#define SEQ_STEPS 64
#define SEQ_DR_BYTES (SEQ_STEPS * 3)                                  /* drums: 3 bytes a step, 10 pads x 2 bits */
#define SEQ_NEX 40                                                    /* drums: edited hits a pattern holds */
#define SEQ_NSTRIDE 5                                                 /* PLVKS: 5 bytes a step */
#define SEQ_PAT_BYTES (SEQ_DR_BYTES + SEQ_NEX * 8)                    /* one pattern (PLVKS: 64 x 5 bytes, the rest unused) */
enum { SEQ_TEMPO, SEQ_SWING, SEQ_LEN, SEQ_RATE, SEQ_PATTERN, SEQ_SYNC, SEQ_HIT, SEQ_ACC, SEQ_NPARAMS };
/* SEQ_TEMPO 0..200 (40..240 BPM), SEQ_SWING 0..127 (50..75 %), SEQ_LEN 0..3 (16, 32, 48, 64 steps),
 * SEQ_RATE 0..2 (1/8, 1/16, 1/32 a step), SEQ_PATTERN (the slot: not used here), SEQ_SYNC 0 internal, 1 MIDI clock,
 * SEQ_HIT, SEQ_ACC: velocities of a hit and an accent (PLVKS: SEQ_HIT is the velocity a new step starts with) */
enum { SEQ_STOP, SEQ_START, SEQ_CONTINUE, SEQ_CLOCK };
enum { SEQ_KIND_PLVKS, SEQ_KIND_DRUMS };
#define SEQ_NONE 255u

typedef struct { uint8_t tune, vel, delay, ratchet, chance; } seq_hit_t;  /* tune 0..48 half semitones (24: none), vel 1..127, delay 0..15 (of 32 of a step), ratchet 0..3 (1..4 hits), chance 0..15 (15: always; else (chance + 1)/16) */
typedef struct { uint8_t on, n1, has2, n2, len, delay, amp, slide, chance; } seq_note_t;   /* notes 0..127, len 0..15 (1/16 .. 16/16 of a step), delay 0..15, amp 1..127, slide (glide in from the note before, legato), chance 0..15 */
typedef struct { float t; uint8_t used, note, vel; } seq_ev_t;        /* a note on (vel > 0; bit 7: legato) or off, t samples from the block start */
typedef struct {
    volatile uint8_t run, pos;           /* running; the step last played (SEQ_NONE: none yet) */
    uint8_t kind, tempo, swing, len, rate, sync, hit, acc;
    float next_in, est;                  /* samples to the next step; one clock's length, measured */
    uint32_t clk, since, rng;            /* (the random numbers of the chance) */
    uint8_t tied;                        /* the notes of the step before are held on for the slide into this one */
    uint8_t prev[2];                     /* the notes of the step before (255: none), for a slide */
    drums_bank_t *d; polivoks_bank_t *pv;
    seq_ev_t ev[8];
    uint8_t snd[6];                      /* PLVKS notes switched on by the sequencer (255: none) */
} seq_t;
void seq_init(seq_t *q, drums_bank_t *d, polivoks_bank_t *pv);
void seq_kind(seq_t *q, unsigned kind);                              /* which player: stops it */
void seq_set(seq_t *q, unsigned parameter, unsigned value);
void seq_command(seq_t *q, unsigned command);                        /* SEQ_STOP, SEQ_START, SEQ_CONTINUE */
void seq_clock_in(seq_t *q, const uint8_t *pat);                     /* one MIDI clock; only when running with sync */
void seq_block(seq_t *q, const uint8_t *pat);                        /* the clock moves on by one audio block */

/* editing a pattern (the main loop; the interrupt may be reading it) */
unsigned seq_get(const uint8_t *pat, unsigned step, unsigned pad);   /* drums: 0 off, 1 hit, 2 accent */
void seq_put(uint8_t *pat, unsigned step, unsigned pad, unsigned state);
int seq_hit_get(const uint8_t *pat, unsigned step, unsigned pad, seq_hit_t *h);   /* 1: this hit is edited */
int seq_hit_put(uint8_t *pat, unsigned step, unsigned pad, const seq_hit_t *h);   /* 0, or -1 when the table is full */
void seq_hit_del(uint8_t *pat, unsigned step, unsigned pad);
void seq_note_get(const uint8_t *pat, unsigned step, seq_note_t *r);
void seq_note_put(uint8_t *pat, unsigned step, const seq_note_t *r);
void seq_copy_page(uint8_t *pat, unsigned kind, unsigned from, unsigned to);      /* 16 steps (and their edits) */
void seq_clear(uint8_t *pat);
void seq_clear_row(uint8_t *pat, unsigned pad);                                   /* drums */
#endif
