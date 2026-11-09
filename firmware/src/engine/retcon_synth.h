/* SPDX-License-Identifier: GPL-3.0-only */
/* retcon_synth.h - the master sound engine:
 *
 *   4 voices -> sum -> tape saturation -> modulated tape/BBD delay -> hiss -> DC block -> out
 *
 * Tape instability is one shared "motor": wow and flutter bend every voice together and jitter the delay's
 * read pointer, as on a real machine. A 16-step sequencer plays the voices.
 *
 * 24 parameters in the six panel pages (4 knobs each); each is 0..1 here (the panel gives 0..127).
 * Everything in this file is float DSP: call it from the audio interrupt only. */
#ifndef RC_SYNTH_H
#define RC_SYNTH_H
#include "../dsp/tape_fx.h"
#include "../dsp/tape_instability.h"
#include "sequencer.h"
#include "voice_allocator.h"

enum {
    RC_P_WAVE_A = 0, RC_P_WAVE_B, RC_P_DETUNE, RC_P_OSC_MIX,        /* P1 OSC:  wave A / wave B (4 zones + morph), detune +/-100 cents, A<->B */
    RC_P_CUTOFF, RC_P_RES, RC_P_ENV_AMT, RC_P_FILT_MODE,            /* P2 FILT: 40 Hz..12 kHz, resonance, env +/-5 octaves, LP/BP/HP */
    RC_P_WOW, RC_P_FLUTTER, RC_P_SATURATION, RC_P_HISS,             /* P3 TAPE */
    RC_P_DLY_TIME, RC_P_DLY_FDBK, RC_P_DLY_DAMP, RC_P_DLY_MIX,      /* P4 DLY:  20..480 ms, 0..95 %, 800 Hz..9 kHz, mix */
    RC_P_ATTACK, RC_P_DECAY, RC_P_SUSTAIN, RC_P_RELEASE,            /* P5 ENV:  1 ms..2 s, 5 ms..4 s, level, 5 ms..6 s */
    RC_P_SEQ_GATE, RC_P_SEQ_TEMPO, RC_P_SEQ_MODE, RC_P_SEQ_SWING,   /* P6 SEQ:  gate 10..100 %, 60..200 BPM, FWD/REV/PING/RAND, swing 0..50 % */
    RC_P_COUNT
};

/* panel defaults (0..127), matching the spec's example screens */
static const uint8_t RC_DEFAULT[RC_P_COUNT] = {
    48, 32, 68, 64,          /* pulse at 50 % PWM, ~saw, +7 cents, 50/50 */
    76, 83, 89, 0,           /* 1.2 kHz, 65 %, +2 octaves of envelope, low-pass */
    53, 23, 32, 19,          /* wow 42 %, flutter 18 %, saturation, hiss */
    72, 74, 60, 38,          /* 280 ms, 55 %, 2.5 kHz, mix */
    45, 81, 89, 91,          /* 15 ms, 350 ms, 70 %, 800 ms */
    64, 36, 0, 0             /* gate 55 %, 100 BPM, forward, no swing */
};

static const char *const RC_PARAM_NAME[RC_P_COUNT] = {
    "WAVE A", "WAVE B", "DETUNE", "OSC MIX", "CUTOFF", "RES", "ENV AMT", "MODE", "WOW", "FLUTTER", "SATURATE", "HISS",
    "TIME", "FEEDBACK", "DAMPING", "MIX", "ATTACK", "DECAY", "SUSTAIN", "RELEASE", "GATE LEN", "TEMPO", "SEQ MODE", "SWING"};

typedef struct {
    rc_alloc_t alloc;
    rc_tape_t tape;
    rc_sat_t sat;
    rc_hiss_t hiss;
    rc_delay_t delay;
    rc_dc_t dc;
    rc_seq_t seq;
    rc_vparams_t params;                        /* live values: cutoff, resonance and mix glide to their targets */
    float cutoff_target, res_target, mix_target;
    uint8_t dirty;
    uint8_t ready;
} rc_synth_t;

/* position of 0..1 on an exponential range min * 2^(n * log2_ratio) */
static inline float rc_exp_map(float n, float min_v, float log2_ratio) { return min_v * rc_exp2(n * log2_ratio); }

/* Wave knob: 4 equal zones (tri/saw, pulse, folded sine, reed); the position inside a zone is the morph. */
static inline void rc_wave_from_knob(float n, uint8_t *shape, float *morph)
{
    float pos = n * 4.0f;
    int zone = (int)pos;
    if (zone > 3) zone = 3;
    *shape = (uint8_t)zone;
    *morph = rc_clamp(pos - (float)zone, 0.0f, 1.0f);
}

static inline void rc_synth_set_param(rc_synth_t *s, unsigned id, float n)
{
    rc_vparams_t *p = &s->params;
    if (id >= RC_P_COUNT) return;
    n = rc_clamp(rc_sanitize(n), 0.0f, 1.0f);
    switch (id) {
    case RC_P_WAVE_A: rc_wave_from_knob(n, &p->shape_a, &p->morph_a); s->dirty = 1; break;
    case RC_P_WAVE_B: rc_wave_from_knob(n, &p->shape_b, &p->morph_b); s->dirty = 1; break;
    case RC_P_DETUNE: p->detune_cents = (n * 2.0f - 1.0f) * 100.0f; s->dirty = 1; break;
    case RC_P_OSC_MIX: s->mix_target = n; break;

    case RC_P_CUTOFF: s->cutoff_target = rc_exp_map(n, 40.0f, 8.22881869f); break;     /* log2(300) */
    case RC_P_RES: s->res_target = n; break;
    case RC_P_ENV_AMT: p->env_amount = n * 2.0f - 1.0f; break;
    case RC_P_FILT_MODE: {
        int m = (int)(n * 3.0f);
        p->filter_mode = (uint8_t)(m > 2 ? 2 : m);
        s->dirty = 1;
        break;
    }

    case RC_P_WOW: s->tape.wow_amount = n; p->drift_cents = 0.5f + 4.0f * n; break;  /* per-voice drift follows the wow depth */
    case RC_P_FLUTTER: s->tape.flutter_amount = n; break;
    case RC_P_SATURATION: rc_sat_set(&s->sat, n); break;
    case RC_P_HISS: rc_hiss_set(&s->hiss, n); break;

    case RC_P_DLY_TIME: rc_delay_time_ms(&s->delay, 20.0f + n * 460.0f); break;
    case RC_P_DLY_FDBK: rc_delay_feedback(&s->delay, n * 0.95f); break;
    case RC_P_DLY_DAMP: rc_delay_damp_hz(&s->delay, rc_exp_map(n, 800.0f, 3.49185f)); break;   /* log2(11.25) */
    case RC_P_DLY_MIX: rc_delay_mix(&s->delay, n); break;

    case RC_P_ATTACK: p->attack_ms = rc_exp_map(n, 1.0f, 10.96578f); s->dirty = 1; break;     /* log2(2000) */
    case RC_P_DECAY: p->decay_ms = rc_exp_map(n, 5.0f, 9.64386f); s->dirty = 1; break;        /* log2(800) */
    case RC_P_SUSTAIN: p->sustain = n; s->dirty = 1; break;
    case RC_P_RELEASE: p->release_ms = rc_exp_map(n, 5.0f, 10.22882f); s->dirty = 1; break;   /* log2(1200) */

    case RC_P_SEQ_GATE: s->seq.gate_pct = (uint8_t)(10.0f + n * 90.0f + 0.5f); break;
    case RC_P_SEQ_TEMPO: s->seq.bpm = (uint16_t)(60.0f + n * 140.0f + 0.5f); break;
    case RC_P_SEQ_MODE: {
        int m = (int)(n * 4.0f);
        s->seq.mode = (uint8_t)(m > 3 ? 3 : m);
        break;
    }
    default: s->seq.swing_pct = (uint8_t)(n * 50.0f + 0.5f); break;                          /* RC_P_SEQ_SWING */
    }
}

static inline void rc_synth_param127(rc_synth_t *s, unsigned id, unsigned v)
{
    rc_synth_set_param(s, id, (float)(v > 127u ? 127u : v) * (1.0f / 127.0f));
}

/* line: RC_DELAY_SAMPLES zeroed int16 (the platform's .pool) */
static inline void rc_synth_init(rc_synth_t *s, int16_t *line)
{
    unsigned i;
    rc_alloc_init(&s->alloc);
    rc_tape_init(&s->tape, 0x5EEDu);
    rc_sat_init(&s->sat);
    rc_hiss_init(&s->hiss);
    rc_delay_init(&s->delay, line);
    rc_dc_init(&s->dc);
    rc_seq_init(&s->seq);
    rc_vparams_init(&s->params);
    for (i = 0; i < RC_P_COUNT; i++) rc_synth_param127(s, i, RC_DEFAULT[i]);
    s->params.cutoff_hz = s->cutoff_target;     /* no glide from the VoiceParams defaults at start */
    s->params.resonance = s->res_target;
    s->params.osc_mix = s->mix_target;
    rc_alloc_apply(&s->alloc, &s->params);
    s->dirty = 0;
    s->ready = 1;
}

/* MIDI-style: velocity 1..127 (0 is a note off) */
static inline void rc_synth_note_on(rc_synth_t *s, unsigned note, unsigned vel)
{
    if (!vel) { rc_alloc_note_off(&s->alloc, (uint8_t)note); return; }
    rc_alloc_note_on(&s->alloc, (uint8_t)note, (float)(vel > 127u ? 127u : vel) * (1.0f / 127.0f));
}
static inline void rc_synth_note_off(rc_synth_t *s, unsigned note) { rc_alloc_note_off(&s->alloc, (uint8_t)note); }
static inline void rc_synth_all_off(rc_synth_t *s)
{
    int n = rc_seq_stop(&s->seq);
    (void)n;
    rc_alloc_all_off(&s->alloc);
}
static inline void rc_synth_seq_run(rc_synth_t *s, int run)
{
    if (run) { if (!s->seq.run) rc_seq_start(&s->seq); }
    else { int n = rc_seq_stop(&s->seq); if (n >= 0) rc_alloc_note_off(&s->alloc, (uint8_t)n); }
}

/* Renders n mono samples (float, nominally -1..1) into out. */
static inline void rc_synth_render(rc_synth_t *s, float *out, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done, i;
        float mixbuf[RC_CTRL_DIV], tape_cents;
        rc_seq_ev_t ev;
        int v;
        if (chunk > (uint32_t)RC_CTRL_DIV) chunk = (uint32_t)RC_CTRL_DIV;

        /* ---- control tick: tape motor, glides, voice parameters, sequencer clock */
        rc_tape_tick(&s->tape);
        s->params.cutoff_hz += (s->cutoff_target - s->params.cutoff_hz) * 0.2f;
        s->params.resonance += (s->res_target - s->params.resonance) * 0.2f;
        s->params.osc_mix += (s->mix_target - s->params.osc_mix) * 0.2f;
        if (s->dirty) { rc_alloc_apply(&s->alloc, &s->params); s->dirty = 0; }
        rc_delay_mod(&s->delay, rc_tape_delay_mod(&s->tape));
        tape_cents = rc_tape_cents(&s->tape);
        rc_seq_advance(&s->seq, chunk, &ev);
        if (ev.off >= 0) rc_alloc_note_off(&s->alloc, (uint8_t)ev.off);
        if (ev.on >= 0) rc_alloc_note_on(&s->alloc, (uint8_t)ev.on, 0.8f);

        /* ---- voices */
        for (i = 0; i < (uint32_t)RC_CTRL_DIV; i++) mixbuf[i] = 0.0f;
        for (v = 0; v < RC_POLYPHONY; v++) rc_voice_render(&s->alloc.voices[v], mixbuf, (int)chunk, &s->params, tape_cents);

        /* ---- master chain */
        for (i = 0; i < chunk; i++) {
            float x = mixbuf[i] * RC_MASTER_GAIN;
            x = rc_sat_process(&s->sat, x);
            x = rc_delay_process(&s->delay, x);
            x += rc_hiss_process(&s->hiss);
            x = rc_dc_process(&s->dc, x);
            out[done + i] = rc_sanitize(x);
        }
        done += chunk;
    }
}

#endif
