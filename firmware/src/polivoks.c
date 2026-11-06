/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Formanta Polivoks voice. See polivoks.h.
 * 44.1 kHz unless polivoks_bank_set_rate says otherwise (a plugin host's rate), 128-frame blocks, float DSP, no libc: runs in the
 * audio interrupt. */
#include "polivoks.h"

#define SR_DEFAULT 44100.0f
#define CHUNK 16                /* control-rate update interval, in samples */
#define HELD_MAX 8
#define Q23 8388608.0f

typedef struct { float att, dec, sus, rel; } pv_adsr_t;

struct polivoks_bank {
    float sr, isr;                       /* the sample rate and its inverse (44.1 kHz on the FM-1) */
    uint8_t pot[POLIVOKS_NPARAMS];
    uint8_t pedal, wheel, nheld, have;
    uint8_t held[HELD_MAX], sustained[HELD_MAX];
    int8_t heldcents[HELD_MAX];
    uint8_t stage, fstage;               /* envelope stages: 0 idle, 1 attack, 2 decay / pedestal, 3 after-sound */
    float pitch1, pitch2, target1, target2;   /* semitones (MIDI scale); oscillator I glides, oscillator II only in a slide */
    uint8_t slide;                       /* the last note came in legato (the sequencer's slide): both oscillators glide to it */
    float ph1, ph2, drift1, drift2, dt1, dt2;
    float env, fenv, s1, s2;             /* envelopes, filter integrator states */
    uint32_t rng, chunks;
    float lfo_phase, lfo_hold, lfo_smooth;
    int32_t bend;                        /* Q16 semitones */
    /* derived from pot[] */
    float cutoff, k, envamt, glide, detune, ofs1, ofs2, lfo_inc, d1, d2, df, da;
    float drive, bias, trim, l1, l2, ln, fm, slide_k;
    float vint, vd1, vd2, vdc, vt1, vt2, vtc, vn1, vn2;   /* vintage: slow wander of both oscillators and the cutoff, and a new tuning for every note */
    pv_adsr_t amp, filt;
};

_Static_assert(sizeof(struct polivoks_bank) <= POLIVOKS_BANK_BYTES, "POLIVOKS_BANK_BYTES too small");

static const char *const range_names[] = {"32'", "16'", "8'", "4'", "2'"};
static const char *const wave_names[] = {"Tri", "Saw", "Pls 50", "Pls 30", "Pls 12"};
static const char *const mode_names[] = {"LP", "BP"};
static const char *const lfo_shapes[] = {"Tri", "Sqr", "Step", "Noise"};
static const char *const loop_names[] = {"Off", "Filter", "Amp", "Both"};
static const char *const voice_names[] = {"Mono", "Duo", "Unison"};
static const char *const gate_names[] = {"Off", "On"};
static const fm1_param_t parameters[POLIVOKS_NPARAMS] = {
    {"Cutoff", 127, 62, 0}, {"Reso", 127, 78, 0}, {"Mode", 1, 0, mode_names}, {"EnvAmt", 127, 70, 0},
    {"Range1", 4, 2, range_names}, {"Wave1", 4, 1, wave_names}, {"Level1", 127, 110, 0}, {"FM 2>1", 127, 0, 0},
    {"Range2", 4, 2, range_names}, {"Wave2", 4, 3, wave_names}, {"Level2", 127, 100, 0}, {"Detune", 127, 66, 0},
    {"Attack", 127, 3, 0}, {"Decay", 127, 55, 0}, {"Pedest", 127, 90, 0}, {"Release", 127, 40, 0},
    {"F.Atk", 127, 2, 0}, {"F.Dec", 127, 60, 0}, {"F.Ped", 127, 30, 0}, {"F.Rel", 127, 40, 0},
    {"LfoRate", 127, 60, 0}, {"LfoWave", 3, 0, lfo_shapes}, {"LFO>Osc1", 127, 0, 0}, {"LFO>Osc2", 127, 0, 0},
    {"LFO>Filt", 127, 0, 0}, {"LFO>Amp", 127, 0, 0}, {"Noise", 127, 0, 0}, {"Drive", 127, 70, 0},
    {"Voices", 2, 1, voice_names}, {"Gate", 1, 0, gate_names}, {"Glide", 127, 0, 0}, {"Loop", 3, 0, loop_names}};

const fm1_param_t *polivoks_param(int p) { return (unsigned)p < POLIVOKS_NPARAMS ? parameters + p : 0; }
size_t polivoks_bank_size(void) { return sizeof(struct polivoks_bank); }

/* ---- math helpers (no libm) ---- */
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float absf(float x) { return x < 0 ? -x : x; }
static float pv_exp2(float x)
{
    x = clampf(x, -120.0f, 120.0f);
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float a = (x - (float)n) * 0.6931471805599453f;
    float p = 1.0f + a * (1.0f + a * (0.5f + a * (1.0f / 6.0f + a * (1.0f / 24.0f + a * (1.0f / 120.0f + a / 720.0f)))));
    union { uint32_t u; float f; } s;
    s.u = (uint32_t)(n + 127) << 23;
    return p * s.f;
}
static inline float pv_exp(float x) { return pv_exp2(x * 1.4426950408889634f); }
/* Smooth saturator standing in for the filter core's overload. */
static inline float sat(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
/* tan(pi*fc/sr) for the filter coefficient; Pade, valid to ~fc = 16 kHz (isr = 1 / sr). */
static inline float warp(float fc, float isr)
{
    float x = 3.14159265f * fc * isr, x2 = x * x;
    return x * (105.0f - 10.0f * x2) / (105.0f - 45.0f * x2 + x2 * x2);
}
static inline float noise(uint32_t *s)
{
    uint32_t n = *s; n ^= n << 13; n ^= n >> 17; n ^= n << 5; *s = n;
    return (float)(n >> 8) * (1.0f / 8388608.0f) - 1.0f;
}
static inline float blep(float t, float dt)
{
    if (t < dt) { float x = t / dt; return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}
static inline float wrap1(float p) { return p >= 1.0f ? p - 1.0f : p; }

static float osc(float *phase, float inc, unsigned wave)
{
    float p = *phase, v;
    switch (wave) {
    case POLIVOKS_WAVE_TRI: v = 4.0f * absf(p - 0.5f) - 1.0f; break;
    case POLIVOKS_WAVE_SAW: v = 2.0f * p - 1.0f - blep(p, inc); break;
    default: {                                                  /* pulse: 50 %, 30 %, 12 % */
        float w = wave == POLIVOKS_WAVE_PULSE50 ? 0.5f : wave == POLIVOKS_WAVE_PULSE30 ? 0.3f : 0.12f;
        float mean = 2.0f * w - 1.0f;
        v = (p < w ? 1.0f : -1.0f) + blep(p, inc) - blep(wrap1(p + 1.0f - w), inc);
        v = (v - mean) * (1.0f / (1.0f + absf(mean)));          /* no DC, a similar level */
        break; }
    }
    p += inc; if (p >= 1.0f) p -= 1.0f;
    *phase = p;
    return v;
}

/* ---- parameters ---- */
static void adsr(pv_adsr_t *e, const uint8_t *p, float SR)
{
    e->att = 1.0f - pv_exp(-1.0f / (SR * 0.001f * pv_exp2(p[0] * (1.0f / 127.0f) * 12.2f)));
    e->dec = 1.0f - pv_exp(-1.0f / (SR * 0.005f * pv_exp2(p[1] * (1.0f / 127.0f) * 10.2f)));
    e->sus = p[2] * (1.0f / 127.0f);
    e->rel = 1.0f - pv_exp(-1.0f / (SR * 0.005f * pv_exp2(p[3] * (1.0f / 127.0f) * 10.2f)));
}
static void derive(polivoks_bank_t *b)
{
    const uint8_t *p = b->pot;
    const float SR = b->sr;
    float u;
    b->cutoff = 40.0f * pv_exp2(p[POLIVOKS_CUTOFF] * (1.0f / 127.0f) * 9.0f);
    u = 1.0f - p[POLIVOKS_RESO] * (1.0f / 127.0f);
    b->k = 2.0f * u * u + 0.02f;                                         /* damping: ~2 (none) .. 0.02 (screaming) */
    b->envamt = p[POLIVOKS_ENVAMT] * (1.0f / 127.0f) * 6.0f;
    u = (p[POLIVOKS_DETUNE] - 64) * (1.0f / 63.0f);
    b->detune = (u < 0 ? -1.0f : 1.0f) * u * u * 5.0f;                   /* +-5 semitones (the original: at least +-4) */
    b->ofs1 = ((int)p[POLIVOKS_RANGE1] - 2) * 12.0f;
    b->ofs2 = ((int)p[POLIVOKS_RANGE2] - 2) * 12.0f;
    u = p[POLIVOKS_GLIDE] * (1.0f / 127.0f);
    b->glide = u <= 0.0f ? 1.0f : 1.0f - pv_exp(-(float)CHUNK / (SR * 0.002f * pv_exp2(u * 10.0f)));
    b->slide_k = 1.0f - pv_exp(-(float)CHUNK / (SR * 0.06f));            /* a slide: about 60 ms */
    adsr(&b->amp, p + POLIVOKS_ATTACK, b->sr);
    adsr(&b->filt, p + POLIVOKS_FATTACK, b->sr);
    b->lfo_inc = CHUNK * 0.1f * pv_exp2(p[POLIVOKS_LFORATE] * (1.0f / 127.0f) * 9.2f) / SR;   /* 0.1 .. ~60 Hz */
    u = p[POLIVOKS_LFO_OSC1] * (1.0f / 127.0f); b->d1 = u * u * 14.0f;                       /* semitones */
    u = p[POLIVOKS_LFO_OSC2] * (1.0f / 127.0f); b->d2 = u * u * 14.0f;
    b->df = p[POLIVOKS_LFO_FILTER] * (1.0f / 127.0f) * 4.0f;                                /* octaves */
    b->da = p[POLIVOKS_LFO_AMP] * (1.0f / 127.0f);
    b->l1 = p[POLIVOKS_LEVEL1] * (1.0f / 127.0f);
    b->l2 = p[POLIVOKS_LEVEL2] * (1.0f / 127.0f);
    b->ln = p[POLIVOKS_NOISE] * (1.0f / 127.0f);
    u = p[POLIVOKS_FM] * (1.0f / 127.0f); b->fm = u * u * 3.0f;                              /* octaves per full-scale swing */
    u = p[POLIVOKS_DRIVE] * (1.0f / 127.0f);
    b->drive = 1.0f + 6.0f * u * u;                                      /* the mixer overloading the filter input */
    b->bias = 0.3f * u;                                                  /* ... unevenly: even harmonics */
    b->trim = 1.0f / (1.0f + 0.9f * u);
}

/* ---- notes: the lowest held key plays oscillator I and the highest oscillator II (duo), or the last key both (mono) ---- */
static void update(polivoks_bank_t *b, int trigger)
{
    if (b->nheld) {
        unsigned lo = 0, hi = 0, last = b->nheld - 1u;
        for (unsigned i = 1; i < b->nheld; ++i) {
            if (b->held[i] < b->held[lo]) lo = i;
            if (b->held[i] > b->held[hi]) hi = i;
        }
        if (b->pot[POLIVOKS_VOICES] == POLIVOKS_DUO) {
            b->target1 = b->held[lo] + b->heldcents[lo] * 0.01f;
            b->target2 = b->held[hi] + b->heldcents[hi] * 0.01f;
        } else {
            b->target1 = b->target2 = b->held[last] + b->heldcents[last] * 0.01f;
        }
        b->have = 1;
    }
    if (b->nheld || (b->pot[POLIVOKS_GATE] && b->have)) {                /* gate: a key, or GATE holds the last note */
        if (trigger || !b->stage || b->stage == 3) {
            b->vn1 = noise(&b->rng) * 0.12f; b->vn2 = noise(&b->rng) * 0.12f;      /* (a vintage voice never lands twice on the same pitch) */
            if (!b->stage) { b->pitch1 = b->target1; b->pitch2 = b->target2; b->s1 = b->s2 = 0.0f; b->env = b->fenv = 0.0f; }
            b->stage = b->fstage = 1;
        }
    } else if (b->stage) {
        b->stage = b->fstage = 3;
    }
}
void polivoks_bank_set(polivoks_bank_t *b, unsigned parameter, unsigned value)
{
    if (parameter >= POLIVOKS_NPARAMS) return;
    if (value > parameters[parameter].max) value = parameters[parameter].max;
    b->pot[parameter] = (uint8_t)value;
    derive(b);
    if (parameter == POLIVOKS_VOICES || parameter == POLIVOKS_GATE) update(b, 0);
}
unsigned polivoks_bank_get(const polivoks_bank_t *b, unsigned parameter)
{
    return parameter < POLIVOKS_NPARAMS ? b->pot[parameter] : 0;
}
void polivoks_bank_init(polivoks_bank_t *b)
{
    uint8_t *raw = (uint8_t *)b;
    for (size_t i = 0; i < sizeof *b; ++i) raw[i] = 0;
    for (unsigned i = 0; i < POLIVOKS_NPARAMS; ++i) b->pot[i] = parameters[i].def;
    b->rng = 0x2545f491u;
    b->sr = SR_DEFAULT; b->isr = 1.0f / SR_DEFAULT;
    derive(b);
}
/* The host's sample rate, 8 kHz .. 192 kHz: every time constant, pitch and the filter coefficient follow it. (The slow wander of
 * the drift and vintage and the noise modulator step once a 16-sample chunk, so they run a little faster at higher rates.) */
void polivoks_bank_set_rate(polivoks_bank_t *b, float sample_rate)
{
    b->sr = clampf(sample_rate, 8000.0f, 192000.0f);
    b->isr = 1.0f / b->sr;
    derive(b);
}
void polivoks_bank_panic(polivoks_bank_t *b)
{
    b->stage = b->fstage = 0; b->env = b->fenv = 0; b->s1 = b->s2 = 0;
    b->nheld = 0; b->pedal = 0; b->have = 0;
}

static void drop(polivoks_bank_t *b, unsigned at)
{
    for (unsigned i = at; i + 1 < b->nheld; ++i) {
        b->held[i] = b->held[i + 1]; b->sustained[i] = b->sustained[i + 1]; b->heldcents[i] = b->heldcents[i + 1];
    }
    --b->nheld;
}
static void note_on_off(polivoks_bank_t *b, unsigned note, unsigned velocity, int cents, int legato)
{
    if (note > 127) return;
    unsigned i, found = HELD_MAX;
    for (i = 0; i < b->nheld; ++i) if (b->held[i] == note) { found = i; break; }
    if (velocity) {
        if (found != HELD_MAX) drop(b, found);                           /* re-press: move to the top */
        else if (b->nheld == HELD_MAX) drop(b, 0);                       /* full: forget the oldest */
        if (cents < -100) cents = -100;
        if (cents > 100) cents = 100;
        b->held[b->nheld] = (uint8_t)note; b->sustained[b->nheld] = 0; b->heldcents[b->nheld] = (int8_t)cents;
        ++b->nheld;
        b->slide = (uint8_t)legato;
        update(b, !legato);
    } else if (found != HELD_MAX) {
        if (b->pedal) { b->sustained[found] = 1; return; }
        drop(b, found);
        update(b, 0);
    }
}
void polivoks_bank_note(polivoks_bank_t *b, unsigned note, unsigned velocity, int cents) { note_on_off(b, note, velocity, cents, 0); }
void polivoks_bank_note_legato(polivoks_bank_t *b, unsigned note, unsigned velocity) { note_on_off(b, note, velocity, 0, 1); }
void polivoks_bank_pedal(polivoks_bank_t *b, unsigned down)
{
    b->pedal = down ? 1 : 0;
    if (down) return;
    unsigned out = 0;
    for (unsigned i = 0; i < b->nheld; ++i)
        if (!b->sustained[i]) {
            b->held[out] = b->held[i]; b->sustained[out] = 0; b->heldcents[out] = b->heldcents[i]; ++out;
        }
    b->nheld = (uint8_t)out;
    update(b, 0);
}
void polivoks_bank_vintage(polivoks_bank_t *b, unsigned v) { b->vint = (float)(v > 127u ? 127u : v) * (1.0f / 127.0f); }
void polivoks_bank_bend(polivoks_bank_t *b, int32_t semitones_q16) { b->bend = semitones_q16; }
void polivoks_bank_mod(polivoks_bank_t *b, unsigned value) { b->wheel = (uint8_t)(value > 127 ? 127 : value); }
unsigned polivoks_bank_active(const polivoks_bank_t *b) { return b->stage != 0; }

/* ---- rendering ---- */
/* RC-style exponential ADSR. With loop set, every time the decay settles onto the pedestal the
 * attack restarts, so the envelope cycles for as long as the gate is held. */
static inline void env_step(float *env, uint8_t *stage, const pv_adsr_t *e, unsigned loop)
{
    switch (*stage) {
    case 1:
        *env += (1.15f - *env) * e->att;
        if (*env >= 1.0f) { *env = 1.0f; *stage = 2; }
        break;
    case 2:
        *env += (e->sus - *env) * e->dec;
        if (loop && *env - e->sus < 0.02f + 0.01f * (1.0f - e->sus) && e->sus < 0.97f) *stage = 1;
        break;
    default:
        *env -= *env * e->rel;
        if (*env < 2e-4f) { *env = 0; *stage = 0; }
        break;
    }
}
unsigned polivoks_bank_render(polivoks_bank_t *b, int32_t *out)
{
    if (!b->stage) return 0;
    const unsigned wave1 = b->pot[POLIVOKS_WAVE1], wave2 = b->pot[POLIVOKS_WAVE2], bandpass = b->pot[POLIVOKS_MODE];
    const unsigned loop = b->pot[POLIVOKS_LOOP], shape = b->pot[POLIVOKS_LFOSHAPE];
    const unsigned loop_amp = loop & POLIVOKS_LOOP_AMP, loop_filter = loop & POLIVOKS_LOOP_FILTER;
    const float wheel = b->wheel * (1.0f / 127.0f) * 1.5f;               /* mod wheel: vibrato and filter wobble */
    const float bend = (float)b->bend * (1.0f / 65536.0f);
    const float bias_sat = sat(b->bias), fm = b->fm;
    static float acc[POLIVOKS_FRAMES];   /* audio interrupt only: kept off its stack */
    for (unsigned i = 0; i < POLIVOKS_FRAMES; ++i) acc[i] = 0.0f;

    for (unsigned c = 0; c < POLIVOKS_FRAMES / CHUNK; ++c) {
        /* the modulator: triangle, square, stepped random (sample and hold) or smoothed noise */
        b->lfo_phase += b->lfo_inc;
        if (b->lfo_phase >= 1.0f) { b->lfo_phase -= 1.0f; b->lfo_hold = noise(&b->rng); }
        float lfo;
        switch (shape) {
        case POLIVOKS_LFO_SQR: lfo = b->lfo_phase < 0.5f ? 1.0f : -1.0f; break;
        case POLIVOKS_LFO_RANDOM: lfo = b->lfo_hold; break;
        case POLIVOKS_LFO_NOISE: b->lfo_smooth += (noise(&b->rng) - b->lfo_smooth) * 0.5f; lfo = b->lfo_smooth * 1.6f; break;
        default: lfo = 4.0f * absf(b->lfo_phase - 0.5f) - 1.0f; break;
        }
        lfo = clampf(lfo, -1.0f, 1.0f);
        ++b->chunks;
        /* pitch: glide (oscillator I only), bend, modulation, a little drift */
        {
            float g1 = b->slide && b->slide_k < b->glide ? b->slide_k : b->glide;
            b->pitch1 += (b->target1 - b->pitch1) * g1;
            b->pitch2 += (b->target2 - b->pitch2) * (b->slide ? b->slide_k : 1.0f);
        }
        if (!(b->chunks & 31)) { b->dt1 = noise(&b->rng) * 5.0f; b->dt2 = noise(&b->rng) * 5.0f; }
        b->drift1 += (b->dt1 - b->drift1) * 0.02f;
        b->drift2 += (b->dt2 - b->drift2) * 0.02f;
        float vcut = 0.0f, vp1 = 0.0f, vp2 = 0.0f;
        if (b->vint > 0.0f) {                                            /* vintage: the tuning and the filter wander, slowly, never the same twice */
            if (!(b->chunks & 1023)) { b->vt1 = noise(&b->rng); b->vt2 = noise(&b->rng); b->vtc = noise(&b->rng); }
            b->vd1 += (b->vt1 - b->vd1) * 0.004f; b->vd2 += (b->vt2 - b->vd2) * 0.004f; b->vdc += (b->vtc - b->vdc) * 0.004f;
            vp1 = (b->vd1 * 0.16f + b->vn1) * b->vint; vp2 = (b->vd2 * 0.16f + b->vn2) * b->vint;   /* semitones: +-16 cents drift and +-12 a note, at most */
            vcut = b->vdc * 0.3f * b->vint;                                                      /* octaves */
        }
        float du = b->pot[POLIVOKS_VOICES] == POLIVOKS_UNISON ? b->detune * 0.5f : 0.0f;      /* unison: the detune either side of the note */
        float s1 = b->pitch1 + b->ofs1 - du + bend + lfo * (b->d1 + wheel) + b->drift1 * 0.01f + vp1;
        float s2 = b->pitch2 + b->ofs2 + b->detune - du + bend + lfo * (b->d2 + wheel) + b->drift2 * 0.01f + vp2;
        float inc1 = clampf(8.175799f * pv_exp2(s1 * (1.0f / 12.0f)) * b->isr, 1e-5f, 0.45f);
        float inc2 = clampf(8.175799f * pv_exp2(s2 * (1.0f / 12.0f)) * b->isr, 1e-5f, 0.45f);
        /* the filter: its cutoff follows its envelope and the modulator, never the keys */
        float fc = b->cutoff * pv_exp2(b->envamt * b->fenv + lfo * (b->df + wheel) + vcut);
        float g = warp(clampf(fc, 20.0f, 16000.0f), b->isr), k = b->k;
        float h = 1.0f / (1.0f + k * g + g * g);
        float tremolo = 1.0f - b->da * 0.5f * (1.0f + lfo);
        float level = 0.5f * Q23 * 0.95f * b->trim * tremolo;   /* (the overdriven mixer loses level: make it up) */
        for (unsigned i = 0; i < CHUNK; ++i) {
            env_step(&b->env, &b->stage, &b->amp, loop_amp);
            env_step(&b->fenv, &b->fstage, &b->filt, loop_filter);
            float o2 = osc(&b->ph2, inc2, wave2);
            float i1 = fm > 0.0f ? clampf(inc1 * pv_exp2(fm * o2), 1e-5f, 0.45f) : inc1;   /* oscillator II frequency-modulates I */
            float o1 = osc(&b->ph1, i1, wave1);
            float in = 0.55f * (b->l1 * o1 + b->l2 * o2);
            if (b->ln > 0.0f) in += 0.5f * b->ln * noise(&b->rng);
            in = sat(in * b->drive + b->bias) - bias_sat;                /* the mixer driving the filter, unevenly */
            /* two-integrator state-variable filter; the integrator saturates like the original's core */
            float hp = (in - (k + g) * b->s1 - b->s2) * h;
            float bp = g * hp + b->s1;
            b->s1 = sat(bp + g * hp);
            float lp = g * bp + b->s2;
            b->s2 = lp + g * bp;
            float y = bandpass ? bp * (k + 0.5f) * 1.6f : lp;
            acc[c * CHUNK + i] += y * b->env * level;
        }
        if (!b->stage) { b->s1 = b->s2 = 0.0f; b->fstage = 0; }
    }
    for (unsigned i = 0; i < POLIVOKS_FRAMES; ++i) {
        float s = clampf(acc[i], -Q23, Q23 - 1.0f);
        out[i] += (int32_t)s;
    }
    return 1;
}
