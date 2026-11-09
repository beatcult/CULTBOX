/* SPDX-License-Identifier: GPL-3.0-only */
/* voice.h - one complete voice: dual oscillator -> SVF (cutoff from the filter envelope and keytrack) ->
 * amp envelope, plus a small per-voice analog-style pitch drift on top of the shared tape wow/flutter. */
#ifndef RC_VOICE_H
#define RC_VOICE_H
#include "adsr.h"
#include "dual_osc.h"
#include "svf.h"
#include "tape_instability.h"

/* Parameters shared by all voices; owned by the synth, read by every voice.
 * The "static" ones are pushed into the voices by rc_voice_apply() when they change; cutoff, resonance,
 * osc_mix, env_amount, keytrack and drift_cents are read every control tick. */
typedef struct {
    uint8_t shape_a, shape_b, filter_mode;
    float morph_a, morph_b, detune_cents, osc_mix;
    float cutoff_hz, resonance, env_amount, keytrack;   /* env_amount -1..1 = +/-5 octaves, keytrack 0..1 */
    float attack_ms, decay_ms, sustain, release_ms;
    float drift_cents;
} rc_vparams_t;

static inline void rc_vparams_init(rc_vparams_t *p)
{
    p->shape_a = RC_SHAPE_PULSE; p->shape_b = RC_SHAPE_TRI_SAW;
    p->morph_a = 0.5f; p->morph_b = 1.0f;
    p->detune_cents = 7.0f; p->osc_mix = 0.5f;
    p->filter_mode = RC_SVF_LP;
    p->cutoff_hz = 1200.0f; p->resonance = 0.65f; p->env_amount = 0.4f; p->keytrack = 0.4f;
    p->attack_ms = 15.0f; p->decay_ms = 350.0f; p->sustain = 0.7f; p->release_ms = 800.0f;
    p->drift_cents = 2.0f;
}

typedef struct {
    rc_dual_t osc;
    rc_svf_t filter;
    rc_adsr_t amp_env, filt_env;
    rc_noise_t drift;
    float velocity, base_hz, key_oct;           /* key_oct: octaves above middle C (note 60) */
    uint32_t age;                               /* allocator stamp, larger = newer */
    uint8_t note, gate;
} rc_voice_t;

static inline void rc_voice_init(rc_voice_t *v, uint32_t seed)
{
    rc_dual_init(&v->osc);
    rc_svf_init(&v->filter);
    rc_adsr_init(&v->amp_env);
    rc_adsr_init(&v->filt_env);
    rc_noise_init(&v->drift, seed * 2654435761u + 99u, 0.9f);
    v->note = 0; v->gate = 0; v->velocity = 0.0f; v->base_hz = 0.0f; v->key_oct = 0.0f; v->age = 0;
}

static inline int rc_voice_active(const rc_voice_t *v) { return rc_adsr_active(&v->amp_env); }

/* push the slow-changing parameters into the voice (call when they change) */
static inline void rc_voice_apply(rc_voice_t *v, const rc_vparams_t *p)
{
    v->osc.a.shape = p->shape_a; v->osc.a.morph = rc_clamp(p->morph_a, 0.0f, 1.0f);
    v->osc.b.shape = p->shape_b; v->osc.b.morph = rc_clamp(p->morph_b, 0.0f, 1.0f);
    rc_dual_set_detune(&v->osc, p->detune_cents);
    v->filter.mode = p->filter_mode;
    rc_adsr_set(&v->amp_env, p->attack_ms, p->decay_ms, p->sustain, p->release_ms);
    rc_adsr_set(&v->filt_env, p->attack_ms, p->decay_ms, p->sustain, p->release_ms);
}

static inline void rc_voice_note_on(rc_voice_t *v, uint8_t note, float vel, uint32_t stamp)
{
    int fresh = !rc_voice_active(v);
    v->note = note;
    v->velocity = rc_clamp(vel, 0.0f, 1.0f);
    v->base_hz = rc_midi_hz((float)note);
    v->key_oct = ((float)note - 60.0f) * (1.0f / 12.0f);
    v->age = stamp;
    v->gate = 1;
    if (fresh) {                                /* reset only from silence: a stolen voice keeps its phase (no click) */
        rc_dual_reset(&v->osc);
        rc_svf_reset(&v->filter);
    }
    rc_adsr_on(&v->amp_env);
    rc_adsr_on(&v->filt_env);
}

static inline void rc_voice_note_off(rc_voice_t *v)
{
    v->gate = 0;
    rc_adsr_off(&v->amp_env);
    rc_adsr_off(&v->filt_env);
}

/* Adds n (<= RC_CTRL_DIV) samples into out[]. One call = one control tick: pitch, mix and filter
 * coefficients are updated at its start. */
static inline void rc_voice_render(rc_voice_t *v, float *out, int n, const rc_vparams_t *p, float tape_cents)
{
    float cents, env_depth, oct, gain;
    int i;
    if (!rc_voice_active(v)) return;
    cents = tape_cents + rc_noise_tick(&v->drift) * p->drift_cents;
    rc_dual_set_freq(&v->osc, v->base_hz, cents);
    v->osc.mix = rc_clamp(p->osc_mix, 0.0f, 1.0f);
    env_depth = p->env_amount * 5.0f * (0.5f + 0.5f * v->velocity);
    oct = env_depth * v->filt_env.level + p->keytrack * v->key_oct;
    rc_svf_set(&v->filter, p->cutoff_hz * rc_exp2(oct), p->resonance);
    gain = RC_VOICE_GAIN * v->velocity;
    for (i = 0; i < n; i++) {
        float e = rc_adsr_process(&v->amp_env), s;
        rc_adsr_process(&v->filt_env);
        s = rc_dual_process(&v->osc);
        s = rc_svf_process(&v->filter, s);
        out[i] += s * e * gain;
    }
}

#endif
