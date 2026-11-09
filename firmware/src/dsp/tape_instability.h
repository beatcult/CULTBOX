/* SPDX-License-Identifier: GPL-3.0-only */
/* tape_instability.h - wow (smoothed random walks) + flutter (fast wandering LFO). Control rate:
 * tick() runs once per RC_CTRL_DIV samples.
 *
 * Depth: the spec gives wow +/-6 Hz and flutter +/-1.5 Hz; pitch is applied in cents, so the Hz figures
 * are converted at 440 Hz: 1200*log2(1+6/440) = 23.4 cents, 1200*log2(1+1.5/440) = 5.9 cents. */
#ifndef RC_TAPE_INSTABILITY_H
#define RC_TAPE_INSTABILITY_H
#include "polyblep.h"

#define RC_WOW_MAX_CENTS 23.4f
#define RC_FLUTTER_MAX_CENTS 5.9f

typedef struct { uint32_t s; } rc_rng_t;
static inline void rc_rng_seed(rc_rng_t *r, uint32_t v) { r->s = v ? v : 0x1234567u; }
static inline uint32_t rc_rng_next(rc_rng_t *r)
{
    uint32_t s = r->s;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    r->s = s;
    return s;
}
static inline float rc_rng_bipolar(rc_rng_t *r) { return (float)(int32_t)rc_rng_next(r) * (1.0f / 2147483648.0f); }

/* Picks a new random target `points_per_sec` times a second, glides between them (Catmull-Rom, C1 smooth). */
typedef struct { rc_rng_t rng; float p0, p1, p2, p3, t, inc; } rc_noise_t;

static inline void rc_noise_rate(rc_noise_t *n, float points_per_sec)
{
    n->inc = rc_clamp(points_per_sec * (1.0f / RC_CTRL_RATE), 0.0f, 0.5f);
}
static inline void rc_noise_init(rc_noise_t *n, uint32_t seed, float points_per_sec)
{
    rc_rng_seed(&n->rng, seed);
    n->p0 = rc_rng_bipolar(&n->rng); n->p1 = rc_rng_bipolar(&n->rng);
    n->p2 = rc_rng_bipolar(&n->rng); n->p3 = rc_rng_bipolar(&n->rng);
    n->t = 0.0f;
    rc_noise_rate(n, points_per_sec);
}
static inline float rc_noise_tick(rc_noise_t *n)    /* about -1..1 */
{
    float a, b, c, y;
    n->t += n->inc;
    while (n->t >= 1.0f) {
        n->t -= 1.0f;
        n->p0 = n->p1; n->p1 = n->p2; n->p2 = n->p3;
        n->p3 = rc_rng_bipolar(&n->rng);
    }
    a = -0.5f * n->p0 + 1.5f * n->p1 - 1.5f * n->p2 + 0.5f * n->p3;
    b = n->p0 - 2.5f * n->p1 + 2.0f * n->p2 - 0.5f * n->p3;
    c = -0.5f * n->p0 + 0.5f * n->p2;
    y = ((a * n->t + b) * n->t + c) * n->t + n->p1;
    return rc_clamp(y, -1.0f, 1.0f);
}

typedef struct {
    rc_noise_t wow_a;                           /* slow belt stretch, ~0.23 Hz */
    rc_noise_t wow_b;                           /* capstan eccentricity, ~1.07 Hz */
    rc_noise_t flutter_rate;                    /* wanders the flutter LFO between 5.5 and 8 Hz */
    float flutter_phase;
    float wow_amount, flutter_amount;           /* 0..1 */
    float wow_value, flutter_value;             /* -1..1 */
} rc_tape_t;

static inline void rc_tape_init(rc_tape_t *t, uint32_t seed)
{
    rc_noise_init(&t->wow_a, seed * 2654435761u + 1u, 0.23f);
    rc_noise_init(&t->wow_b, seed * 2246822519u + 7u, 1.07f);
    rc_noise_init(&t->flutter_rate, seed * 3266489917u + 13u, 0.35f);
    t->flutter_phase = 0.0f;
    t->wow_amount = t->flutter_amount = 0.0f;
    t->wow_value = t->flutter_value = 0.0f;
}

static inline void rc_tape_tick(rc_tape_t *t)
{
    float rate = 6.75f + 1.25f * rc_noise_tick(&t->flutter_rate);   /* 5.5 .. 8.0 Hz */
    float tri;
    t->wow_value = 0.65f * rc_noise_tick(&t->wow_a) + 0.35f * rc_noise_tick(&t->wow_b);
    t->flutter_phase += rate * (1.0f / RC_CTRL_RATE);
    if (t->flutter_phase >= 1.0f) t->flutter_phase -= 1.0f;
    tri = 4.0f * rc_abs(t->flutter_phase - 0.5f) - 1.0f;
    t->flutter_value = 0.75f * tri + 0.25f * rc_sin_turns(t->flutter_phase);
}

/* f = f_base * 2^((detune + wow + flutter) / 1200): the wow + flutter part, in cents */
static inline float rc_tape_cents(const rc_tape_t *t)
{
    return t->wow_value * t->wow_amount * RC_WOW_MAX_CENTS + t->flutter_value * t->flutter_amount * RC_FLUTTER_MAX_CENTS;
}
/* the same drift as read-pointer jitter, in samples, for the tape delay (scaled for 44.1 kHz) */
static inline float rc_tape_delay_mod(const rc_tape_t *t)
{
    return t->wow_value * t->wow_amount * 55.0f + t->flutter_value * t->flutter_amount * 8.0f;
}

#endif
