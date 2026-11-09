/* SPDX-License-Identifier: GPL-3.0-only */
/* tape_fx.h - tape saturation, LFSR cassette hiss, DC blocker and a modulated BBD / tape delay on a
 * statically allocated int16 ring (RC_DELAY_SAMPLES, the platform's .pool). */
#ifndef RC_TAPE_FX_H
#define RC_TAPE_FX_H
#include "polyblep.h"

/* ---- saturator */
enum { RC_SAT_RATIONAL = 0, RC_SAT_CUBIC };
typedef struct { float drive; uint8_t mode; } rc_sat_t;

static inline void rc_sat_init(rc_sat_t *s) { s->drive = 0.0f; s->mode = RC_SAT_RATIONAL; }
/* amount 0..1: 0 is a clean pass-through; quadratic, so the first half of the knob is subtle tape glue */
static inline void rc_sat_set(rc_sat_t *s, float amount)
{
    float a = rc_clamp(amount, 0.0f, 1.0f);
    s->drive = a * a * 10.0f;
}
static inline float rc_sat_process(const rc_sat_t *s, float x)
{
    if (s->mode == RC_SAT_RATIONAL)
        return x * (1.0f + s->drive) / (1.0f + rc_abs(x * s->drive));   /* f(1) = 1 for every drive */
    return 1.5f * rc_clip3(x * (1.0f + s->drive) * (2.0f / 3.0f));      /* cubic, unity small-signal gain and peak */
}

/* ---- hiss: 32-bit Galois LFSR (taps 32,31,29,1 -> 0xD0000001, period 2^32-1), RC_HISS_STEPS clocks per
 * sample, then a one-pole low-pass */
typedef struct { uint32_t lfsr; float lp, level; } rc_hiss_t;

static inline void rc_hiss_init(rc_hiss_t *h) { h->lfsr = 0xACE1BEEFu; h->lp = 0.0f; h->level = 0.0f; }
static inline void rc_hiss_set(rc_hiss_t *h, float amount)      /* 0..1 -> up to about -30 dBFS */
{
    float a = rc_clamp(amount, 0.0f, 1.0f);
    h->level = a * a * 0.03f;
}
static inline float rc_hiss_process(rc_hiss_t *h)
{
    uint32_t s = h->lfsr;
    float white;
    int i;
    if (h->level <= 0.0f) return 0.0f;
    for (i = 0; i < RC_HISS_STEPS; i++)
        s = (s >> 1) ^ ((0u - (s & 1u)) & 0xD0000001u);
    h->lfsr = s;
    white = (float)(int32_t)s * (1.0f / 2147483648.0f);
    h->lp += 0.45f * (white - h->lp);                           /* single pole, ~4.7 kHz at 44.1 kHz */
    h->lp = rc_flush(h->lp);
    return h->lp * h->level;
}

/* ---- DC blocker */
typedef struct { float x1, y1; } rc_dc_t;
static inline void rc_dc_init(rc_dc_t *d) { d->x1 = d->y1 = 0.0f; }
static inline float rc_dc_process(rc_dc_t *d, float x)
{
    float y = x - d->x1 + 0.9964f * d->y1;                      /* ~25 Hz corner */
    d->x1 = x;
    d->y1 = rc_flush(y);
    return y;
}

/* ---- delay */
#define RC_DLY_MIN 64.0f
#define RC_DLY_MAX ((float)(RC_DELAY_SAMPLES - 64))
#define RC_DLY_TIME_SLEW 0.00011f       /* per sample: turning TIME bends the pitch like a tape-speed change */
#define RC_DLY_MOD_SLEW 0.015f          /* per sample: smooths the control-rate modulation updates */

typedef struct {
    int16_t *line;                      /* RC_DELAY_SAMPLES, zeroed (the platform zeroes .pool at boot) */
    uint32_t write_pos;
    float delay_target, delay_cur, mod_target, mod_cur;
    float feedback, damp_coef, damp_state, mix;
} rc_delay_t;

static inline void rc_delay_time_ms(rc_delay_t *d, float ms)
{
    d->delay_target = rc_clamp(ms * 0.001f * RC_SAMPLE_RATE, RC_DLY_MIN, RC_DLY_MAX);
}
static inline void rc_delay_feedback(rc_delay_t *d, float fb) { d->feedback = rc_clamp(fb, 0.0f, 0.95f); }
static inline void rc_delay_mix(rc_delay_t *d, float m) { d->mix = rc_clamp(m, 0.0f, 1.0f); }
static inline void rc_delay_mod(rc_delay_t *d, float samples) { d->mod_target = samples; }
/* one-pole low-pass in the feedback path: coef = 1 - exp(-2 pi fc / fs) */
static inline void rc_delay_damp_hz(rc_delay_t *d, float hz)
{
    float fc = rc_clamp(hz, 200.0f, 0.4f * RC_SAMPLE_RATE);
    d->damp_coef = 1.0f - rc_exp2(-RC_TWO_PI * fc * RC_INV_SAMPLE_RATE * RC_LOG2E);
}

/* The buffer is not cleared here (the pool already is); a host test clears it itself. */
static inline void rc_delay_init(rc_delay_t *d, int16_t *line)
{
    d->line = line;
    d->write_pos = 0;
    d->delay_target = 280.0f * 0.001f * RC_SAMPLE_RATE;
    d->delay_cur = d->delay_target;
    d->mod_target = d->mod_cur = 0.0f;
    d->feedback = 0.55f;
    d->damp_state = 0.0f;
    rc_delay_damp_hz(d, 2500.0f);
    d->mix = 0.3f;
}

static inline int16_t rc_to_i16(float x)
{
    float y = rc_clamp(x, -1.0f, 1.0f) * 32767.0f;
    return (int16_t)(y + (y >= 0.0f ? 0.5f : -0.5f));
}

static inline float rc_delay_process(rc_delay_t *d, float in)
{
    float dd, rp, frac, s0, s1, wet, fb;
    int32_t i0, i1;
    d->delay_cur += (d->delay_target - d->delay_cur) * RC_DLY_TIME_SLEW;
    d->mod_cur += (d->mod_target - d->mod_cur) * RC_DLY_MOD_SLEW;

    /* read_pos = write_pos - base_delay + lfo_modulation, linearly interpolated like a BBD */
    dd = rc_clamp(d->delay_cur + d->mod_cur, 2.0f, RC_DLY_MAX);
    rp = (float)d->write_pos - dd;
    if (rp < 0.0f) rp += (float)RC_DELAY_SAMPLES;
    i0 = (int32_t)rp;
    if (i0 >= RC_DELAY_SAMPLES) i0 -= RC_DELAY_SAMPLES;         /* float rounding at the wrap */
    if (i0 < 0) i0 = 0;
    frac = rp - (float)i0;
    i1 = i0 + 1;
    if (i1 >= RC_DELAY_SAMPLES) i1 = 0;
    s0 = (float)d->line[i0] * (1.0f / 32768.0f);
    s1 = (float)d->line[i1] * (1.0f / 32768.0f);
    wet = s0 + (s1 - s0) * frac;

    d->damp_state += d->damp_coef * (wet - d->damp_state);      /* every pass through the loop gets darker */
    d->damp_state = rc_flush(d->damp_state);

    fb = rc_clip3(d->damp_state * d->feedback);                 /* soft-limited: the loop cannot run away */
    d->line[d->write_pos] = rc_to_i16(rc_sanitize(in + fb));
    if (++d->write_pos >= (uint32_t)RC_DELAY_SAMPLES) d->write_pos = 0;
    return in * (1.0f - d->mix) + d->damp_state * d->mix;
}

#endif
