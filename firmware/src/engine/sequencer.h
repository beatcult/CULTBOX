/* SPDX-License-Identifier: GPL-3.0-only */
/* sequencer.h - RETCON-1's 16-step sequencer (one note per step, 1/16 notes). It runs in the audio
 * interrupt on the engine's control tick (RC_CTRL_DIV samples = 0.36 ms of timing resolution), so the clock
 * is the sample clock. Integer timing only. */
#ifndef RC_SEQUENCER_H
#define RC_SEQUENCER_H
#include "../retcon_config.h"

#define RC_SEQ_STEPS 16
enum { RC_SEQ_FWD = 0, RC_SEQ_REV, RC_SEQ_PING, RC_SEQ_RAND };

typedef struct {
    uint16_t on;                                /* gate per step */
    uint8_t note[RC_SEQ_STEPS];                 /* MIDI note per step */
    uint8_t gate_pct, mode, swing_pct;          /* gate length 10..100 %, direction, swing 0..50 % */
    uint16_t bpm;                               /* 60..200 */
    uint8_t run, step, dir, fresh, gate_open, last_note;
    volatile uint8_t pos;                       /* the step that played last, for the panel */
    uint32_t to_step, to_off, rnd;              /* samples until the next step and until the gate closes */
} rc_seq_t;

typedef struct { int16_t on, off; } rc_seq_ev_t;    /* -1 = none */

static inline void rc_seq_init(rc_seq_t *q)
{
    static const uint8_t NOTES[RC_SEQ_STEPS] = {48, 48, 55, 48, 51, 48, 58, 55, 48, 48, 55, 48, 53, 51, 58, 55};
    int i;
    for (i = 0; i < RC_SEQ_STEPS; i++) q->note[i] = NOTES[i];
    q->on = 0x1111u | 0x0404u;
    q->gate_pct = 55; q->mode = RC_SEQ_FWD; q->swing_pct = 0; q->bpm = 100;
    q->run = 0; q->step = 0; q->dir = 1; q->fresh = 1; q->gate_open = 0; q->last_note = 0; q->pos = 0;
    q->to_step = 0; q->to_off = 0; q->rnd = 0x2545F491u;
}

static inline void rc_seq_start(rc_seq_t *q)
{
    q->run = 1; q->fresh = 1; q->dir = 1; q->to_step = 0; q->gate_open = 0;
}

/* stop: returns the note still sounding (or -1) so the engine can release it */
static inline int rc_seq_stop(rc_seq_t *q)
{
    int n = q->gate_open ? (int)q->last_note : -1;
    q->run = 0; q->gate_open = 0;
    return n;
}

static inline unsigned rc_seq_next(rc_seq_t *q)
{
    unsigned s = q->step;
    if (q->fresh) { q->fresh = 0; q->dir = 1; return q->mode == RC_SEQ_REV ? 15u : 0u; }
    switch (q->mode) {
    case RC_SEQ_REV: s = (s - 1u) & 15u; break;
    case RC_SEQ_PING:
        if (q->dir) { if (s >= 15u) { q->dir = 0; s = 14u; } else s++; }
        else { if (s == 0u) { q->dir = 1; s = 1u; } else s--; }
        break;
    case RC_SEQ_RAND:
        q->rnd ^= q->rnd << 13; q->rnd ^= q->rnd >> 17; q->rnd ^= q->rnd << 5;
        s = (q->rnd >> 8) & 15u;
        break;
    default: s = (s + 1u) & 15u; break;
    }
    return s;
}

/* The time from step s to the next one: a 1/16 note at the tempo, with swing (even steps long, odd short). */
static inline uint32_t rc_seq_len(const rc_seq_t *q, unsigned s)
{
    uint32_t base = 661500u / (uint32_t)(q->bpm ? q->bpm : 100u);       /* 44100 * 60 / bpm / 4 */
    return base * (s & 1u ? 100u - q->swing_pct : 100u + q->swing_pct) / 100u;
}

/* Advance by n samples (n <= RC_CTRL_DIV). At most one gate-off and one step start fall inside. */
static inline void rc_seq_advance(rc_seq_t *q, uint32_t n, rc_seq_ev_t *ev)
{
    ev->on = ev->off = -1;
    if (!q->run) return;
    if (q->gate_open) {
        if (q->to_off <= n) { ev->off = q->last_note; q->gate_open = 0; }
        else q->to_off -= n;
    }
    if (q->to_step <= n) {
        unsigned s = rc_seq_next(q);
        uint32_t len = rc_seq_len(q, s), late = n - q->to_step, gl;
        q->step = (uint8_t)s;
        q->pos = (uint8_t)s;
        q->to_step = len > late ? len - late : 1u;
        if (q->on >> s & 1u) {
            if (q->gate_open) ev->off = q->last_note;       /* a retrigger closes the old note first */
            gl = len * q->gate_pct / 100u;
            if (gl < 2u) gl = 2u;
            ev->on = q->note[s];
            q->last_note = q->note[s];
            q->gate_open = 1;
            q->to_off = gl > late ? gl - late : 1u;
        }
    } else {
        q->to_step -= n;
    }
}

#endif
