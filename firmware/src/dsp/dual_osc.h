/* SPDX-License-Identifier: GPL-3.0-only */
/* dual_osc.h - osc A + osc B + mix + detune. */
#ifndef RC_DUAL_OSC_H
#define RC_DUAL_OSC_H
#include "algo_osc.h"

/* ---- dual oscillator */
typedef struct {
    rc_osc_t a, b;
    float mix;                                  /* 0 = A only, 1 = B only */
    float detune_ratio;                         /* B's frequency ratio, cached from cents */
} rc_dual_t;

static inline void rc_dual_init(rc_dual_t *d)
{
    rc_osc_init(&d->a, RC_SHAPE_TRI_SAW, 0.5f);
    rc_osc_init(&d->b, RC_SHAPE_TRI_SAW, 0.5f);
    d->mix = 0.5f;
    d->detune_ratio = 1.0f;
}
static inline void rc_dual_set_detune(rc_dual_t *d, float cents) { d->detune_ratio = rc_exp2(cents * (1.0f / 1200.0f)); }
static inline void rc_dual_reset(rc_dual_t *d) { d->a.phase = d->b.phase = 0.0f; d->a.reed_lp = d->b.reed_lp = 0.0f; }

/* base_hz: the note; mod_cents: tape wow, flutter and drift for both oscillators.
 * f = f_base * 2^(cents / 1200); detune goes on B on top. */
static inline void rc_dual_set_freq(rc_dual_t *d, float base_hz, float mod_cents)
{
    float fa = base_hz * rc_exp2(mod_cents * (1.0f / 1200.0f));
    rc_osc_set_freq(&d->a, fa);
    rc_osc_set_freq(&d->b, fa * d->detune_ratio);
}

static inline float rc_dual_process(rc_dual_t *d)
{
    float sa = rc_osc_process(&d->a), sb = rc_osc_process(&d->b);
    return sa + (sb - sa) * d->mix;
}

#endif
