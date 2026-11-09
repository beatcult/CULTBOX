/* SPDX-License-Identifier: GPL-3.0-only */
/* svf.h - 2-pole Cytomic (Simper) trapezoidal state-variable filter.
 *
 *   g = tan(pi fc / fs)       k = 2 - 2*resonance  (k in [0.05, 2.0])
 *   a1 = 1 / (1 + g (g + k))  a2 = g a1            a3 = g a2
 *   v3 = v0 - ic2eq
 *   v1 = a1 ic1eq + a2 v3            (band-pass node)
 *   v2 = ic2eq + a2 ic1eq + a3 v3    (low-pass node)
 *   ic1eq = 2 v1 - ic1eq             ic2eq = 2 v2 - ic2eq
 *   LP = v2    BP = v1 * k (unity peak, raw v1 peaks at 1/k)    HP = v0 - k v1 - v2
 *
 * The spec's literal update equations mix the two integrator states; this is the canonical form of the
 * same topology with the same g, k and LP/BP/HP outputs. */
#ifndef RC_SVF_H
#define RC_SVF_H
#include "polyblep.h"

enum { RC_SVF_LP = 0, RC_SVF_BP, RC_SVF_HP };

typedef struct {
    float g, k, a1, a2, a3, ic1eq, ic2eq;
    uint8_t mode;
} rc_svf_t;

static inline void rc_svf_reset(rc_svf_t *f) { f->ic1eq = 0.0f; f->ic2eq = 0.0f; }

/* Control rate: once per RC_CTRL_DIV samples, not per sample. */
static inline void rc_svf_set(rc_svf_t *f, float fc_hz, float resonance)
{
    float fc = rc_clamp(fc_hz, 20.0f, 0.38f * RC_SAMPLE_RATE);   /* keeps rc_tan in its accurate range */
    f->g = rc_tan(RC_PI * fc * RC_INV_SAMPLE_RATE);
    f->k = rc_clamp(2.0f - 2.0f * resonance, 0.05f, 2.0f);
    f->a1 = 1.0f / (1.0f + f->g * (f->g + f->k));
    f->a2 = f->g * f->a1;
    f->a3 = f->g * f->a2;
}

static inline void rc_svf_init(rc_svf_t *f)
{
    rc_svf_reset(f);
    f->mode = RC_SVF_LP;
    rc_svf_set(f, 1000.0f, 0.0f);
}

static inline float rc_svf_process(rc_svf_t *f, float v0)
{
    float v3 = v0 - f->ic2eq;
    float v1 = f->a1 * f->ic1eq + f->a2 * v3;
    float v2 = f->ic2eq + f->a2 * f->ic1eq + f->a3 * v3;
    f->ic1eq = 2.0f * v1 - f->ic1eq;
    f->ic2eq = 2.0f * v2 - f->ic2eq;
    if (!rc_is_finite(f->ic1eq) || !rc_is_finite(f->ic2eq)) {    /* NaN/Inf guard: reset, never latch */
        rc_svf_reset(f);
        return 0.0f;
    }
    f->ic1eq = rc_flush(f->ic1eq);
    f->ic2eq = rc_flush(f->ic2eq);
    if (f->mode == RC_SVF_BP) return v1 * f->k;
    if (f->mode == RC_SVF_HP) return v0 - f->k * v1 - v2;
    return v2;
}

#endif
