/* SPDX-License-Identifier: GPL-3.0-only */
/* algo_osc.h - one algorithmic oscillator with four morphable shapes. */
#ifndef RC_ALGO_OSC_H
#define RC_ALGO_OSC_H
#include "polyblep.h"

enum { RC_SHAPE_TRI_SAW = 0, RC_SHAPE_PULSE, RC_SHAPE_FOLD, RC_SHAPE_REED };   /* morph: skew, PWM, fold, brightness */

typedef struct {
    float phase, phase_inc;
    float morph;                                /* 0..1 */
    float reed_lp;                              /* one-pole state of the reed */
    uint8_t shape;
} rc_osc_t;

static inline void rc_osc_init(rc_osc_t *o, unsigned shape, float morph)
{
    o->phase = 0.0f;
    o->phase_inc = 0.0f;
    o->reed_lp = 0.0f;
    o->shape = (uint8_t)shape;
    o->morph = rc_clamp(morph, 0.0f, 1.0f);
}

static inline void rc_osc_set_freq(rc_osc_t *o, float hz)
{
    float inc = hz * RC_INV_SAMPLE_RATE;
    if (inc < 0.0f) inc = 0.0f;
    if (inc > 0.49f) inc = 0.49f;               /* Nyquist clamp */
    o->phase_inc = inc;
}

/* Triangular wavefolder: identity inside [-1,1], reflects at +/-1 repeatedly. */
static inline float rc_fold(float x)
{
    float t = x * 0.25f + 0.25f;
    t -= rc_floor(t);
    return 1.0f - rc_abs(t * 4.0f - 2.0f);
}

static inline float rc_osc_process(rc_osc_t *o)
{
    float out = 0.0f, ph = o->phase, dt = o->phase_inc, m = o->morph;
    switch (o->shape) {
    case RC_SHAPE_TRI_SAW: {
        float raw_saw = 2.0f * ph - 1.0f;
        float saw = raw_saw - rc_blep(ph, dt);
        float tri = 2.0f * rc_abs(raw_saw) - 1.0f;
        out = tri * (1.0f - m) + saw * m;       /* 0 = triangle, 1 = sawtooth */
        break;
    }
    case RC_SHAPE_PULSE: {
        float pw = 0.05f + 0.90f * m;           /* duty cycle 5%..95% */
        float raw = ph < pw ? 1.0f : -1.0f;
        float p1 = rc_blep(ph, dt);             /* rising edge at t = 0 */
        float ph2 = ph - pw, p2, dc;            /* falling edge at t = pw */
        if (ph2 < 0.0f) ph2 += 1.0f;
        p2 = rc_blep(ph2, dt);
        dc = 2.0f * pw - 1.0f;                  /* remove the DC and keep narrow pulses in range */
        out = (raw + p1 - p2 - dc) / (1.0f + rc_abs(dc));
        break;
    }
    case RC_SHAPE_FOLD: {
        float aa = rc_clamp(1.0f - dt * 6.0f, 0.0f, 1.0f);   /* less fold towards Nyquist */
        float drive = 1.0f + m * 4.0f * aa;
        out = rc_fold(rc_sin_turns(ph) * drive);
        break;
    }
    default: {                                  /* RC_SHAPE_REED: 50% square, both edges band-limited, one-pole damper */
        float raw = ph < 0.5f ? 1.0f : -1.0f;
        float ph2 = ph + 0.5f, sq, a;
        if (ph2 >= 1.0f) ph2 -= 1.0f;
        sq = 0.7f * (raw + rc_blep(ph, dt) - rc_blep(ph2, dt));
        a = 0.08f + 0.60f * m;
        o->reed_lp += a * (sq - o->reed_lp);
        o->reed_lp = rc_flush(o->reed_lp);
        out = o->reed_lp;
        break;
    }
    }
    ph += dt;
    if (ph >= 1.0f) ph -= 1.0f;
    o->phase = ph;
    return out;
}

#endif
