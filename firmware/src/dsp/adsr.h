/* SPDX-License-Identifier: GPL-3.0-only */
/* adsr.h - float ADSR with exponential (default) or linear segments.
 * Exponential: the attack charges towards 1.2 and ends on crossing 1.0 (analog RC style); decay and
 * release are one-pole decays timed to cover ~60 dB in the stated time. */
#ifndef RC_ADSR_H
#define RC_ADSR_H
#include "polyblep.h"

enum { RC_ENV_IDLE = 0, RC_ENV_ATTACK, RC_ENV_DECAY, RC_ENV_SUSTAIN, RC_ENV_RELEASE };

typedef struct {
    float level, sustain;
    float a_coef, d_coef, r_coef;               /* exponential */
    float a_inc, d_dec, r_dec, r_samples;       /* linear, per sample */
    uint8_t stage, linear;
} rc_adsr_t;

/* Times in ms, sustain 0..1. Control rate (uses rc_exp2). */
static inline void rc_adsr_set(rc_adsr_t *e, float attack_ms, float decay_ms, float sustain, float release_ms)
{
    float spms = RC_SAMPLE_RATE * 0.001f;
    float na = attack_ms * spms, nd = decay_ms * spms, nr = release_ms * spms;
    e->sustain = rc_clamp(sustain, 0.0f, 1.0f);
    if (na < 1.0f) na = 1.0f;
    if (nd < 1.0f) nd = 1.0f;
    if (nr < 1.0f) nr = 1.0f;
    e->a_coef = 1.0f - rc_exp2(-1.79f * RC_LOG2E / na);       /* 0 -> 1.2 crosses 1.0 at 1.79 tau */
    e->d_coef = 1.0f - rc_exp2(-6.9078f * RC_LOG2E / nd);     /* ~60 dB over decay_ms */
    e->r_coef = 1.0f - rc_exp2(-6.9078f * RC_LOG2E / nr);
    e->a_inc = 1.0f / na;
    e->d_dec = (1.0f - e->sustain) / nd;
    e->r_samples = nr;
}

static inline void rc_adsr_init(rc_adsr_t *e)
{
    e->stage = RC_ENV_IDLE;
    e->level = 0.0f;
    e->linear = 0;
    rc_adsr_set(e, 15.0f, 350.0f, 0.7f, 800.0f);
}

static inline void rc_adsr_on(rc_adsr_t *e) { e->stage = RC_ENV_ATTACK; }    /* retriggers from the current level: no click */
static inline void rc_adsr_off(rc_adsr_t *e)
{
    if (e->stage == RC_ENV_IDLE) return;
    e->stage = RC_ENV_RELEASE;
    e->r_dec = e->level / e->r_samples;
}
static inline int rc_adsr_active(const rc_adsr_t *e) { return e->stage != RC_ENV_IDLE; }

static inline float rc_adsr_process(rc_adsr_t *e)
{
    switch (e->stage) {
    case RC_ENV_IDLE:
        return 0.0f;
    case RC_ENV_ATTACK:
        if (e->linear) e->level += e->a_inc;
        else e->level += (1.2f - e->level) * e->a_coef;
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = RC_ENV_DECAY; }
        break;
    case RC_ENV_DECAY:
        if (e->linear) {
            e->level -= e->d_dec;
            if (e->level <= e->sustain) { e->level = e->sustain; e->stage = RC_ENV_SUSTAIN; }
        } else {
            e->level += (e->sustain - e->level) * e->d_coef;
            if (e->level - e->sustain < 1.0e-4f) { e->level = e->sustain; e->stage = RC_ENV_SUSTAIN; }
        }
        break;
    case RC_ENV_SUSTAIN:
        e->level += (e->sustain - e->level) * 0.005f;           /* glides if the knob moves while held */
        break;
    default:                                                    /* RC_ENV_RELEASE */
        if (e->linear) e->level -= e->r_dec;
        else e->level -= e->level * e->r_coef;
        if (e->level < 1.0e-4f || e->level <= 0.0f) { e->level = 0.0f; e->stage = RC_ENV_IDLE; }
        break;
    }
    return e->level;
}

#endif
