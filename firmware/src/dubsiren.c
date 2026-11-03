/* SPDX-License-Identifier: GPL-3.0-only */
/* Dub siren. See dubsiren.h. Fixed 44.1 kHz, 128-frame blocks, float DSP, no libc. */
#include "dubsiren.h"

#define DS_SR 44100.0f
#define DS_CHUNK 16
#define DS_Q23 8388608.0f
#define DS_HELD 8
#define DS_TBL 256

struct dubsiren_bank {
    uint8_t pot[SIREN_NPARAMS];
    uint8_t held[DS_HELD], nheld, active, gate, wheel;
    float semis, target, bend;                  /* keyboard pitch in semitones from Tune (glided), bend */
    float ph, lfo_ph, env, s1, s2;
    /* derived */
    float tune, rate, depth, cutoff, k, sweep, glide, rel, level;
    uint8_t shape, tunemode, hold;
    float vint, vd, vc, vtd, vtc;                  /* vintage: the pitch and the filter wander */
    uint32_t rng, nch;
};
_Static_assert(sizeof(struct dubsiren_bank) <= DUBSIREN_BANK_BYTES, "DUBSIREN_BANK_BYTES too small");

static const char *const ds_shapes[] = {"Sine", "Square", "SawDn", "Tri", "Warble", "NarPls", "Stairs", "S&H"};
static const char *const ds_sweepmodes[] = {"Filter", "+Tune"};
static const char *const ds_hold[] = {"Off", "On"};
static const fm1_param_t ds_params[SIREN_NPARAMS] = {
    {"Tune", 127, 57, 0}, {"Rate", 127, 84, 0}, {"Depth", 127, 76, 0}, {"Shape", 7, 0, ds_shapes},
    {"Cutoff", 127, 83, 0}, {"Reso", 127, 26, 0}, {"Sweep", 127, 64, 0}, {"SweepTo", 1, 1, ds_sweepmodes},
    {"Decay", 127, 28, 0}, {"Hold", 1, 0, ds_hold}, {"Glide", 127, 0, 0}, {"Volume", 127, 108, 0}};
const fm1_param_t *dubsiren_param(int p) { return (unsigned)p < SIREN_NPARAMS ? ds_params + p : 0; }

static float ds_tbl[4][DS_TBL];                 /* the four bank-B shapes: dubmarine's periodic waves, peak 1 */
static uint8_t ds_tbl_ready;

static inline float ds_clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float ds_abs(float x) { return x < 0 ? -x : x; }
static float ds_exp2(float x)
{
    x = ds_clamp(x, -60.0f, 60.0f);
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float a = (x - (float)n) * 0.6931471805599453f;
    float p = 1.0f + a * (1.0f + a * (0.5f + a * (1.0f / 6.0f + a * (1.0f / 24.0f + a * (1.0f / 120.0f + a / 720.0f)))));
    union { uint32_t u; float f; } s;
    s.u = (uint32_t)(n + 127) << 23;
    return p * s.f;
}
static inline float ds_exp(float x) { return ds_exp2(x * 1.4426950408889634f); }
static float ds_sin(float turns)                /* sine of 2*pi*turns, ~0.1 % */
{
    turns -= (float)(int)turns; if (turns < 0) turns += 1.0f;
    float x = turns < 0.5f ? turns : turns - 1.0f;
    float y = 4.0f * x * (1.0f - 2.0f * ds_abs(x));
    return 0.775f * y + 0.225f * y * ds_abs(y);
}
static inline float ds_sat(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
static inline float ds_warp(float fc)
{
    float x = 3.14159265f * fc * (1.0f / DS_SR), x2 = x * x;
    return x * (105.0f - 10.0f * x2) / (105.0f - 45.0f * x2 + x2 * x2);
}
static inline float ds_blep(float t, float dt)
{
    if (t < dt) { float x = t / dt; return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}

static void ds_make_tables(void)
{
    for (unsigned w = 0; w < 4; ++w) {
        float peak = 1e-9f;
        for (unsigned n = 0; n < DS_TBL; ++n) {
            float t = (float)n / (float)DS_TBL, v = 0.0f;
            switch (w) {
            case 0: v = 0.75f * ds_sin(t) + 0.35f * ds_sin(2.0f * t); break;                                   /* dual warble */
            case 1: for (unsigned i = 1; i < 16; ++i) v += (2.0f / (3.14159265f * (float)i)) * ds_sin(0.125f * (float)i) * ds_sin((float)i * t) * 1.0f; break;
            case 2: for (unsigned i = 1; i < 32; ++i) v += (1.0f / (float)i) * ds_sin(0.125f * (float)i) * ds_sin((float)i * t); break;
            default: for (unsigned i = 1; i < 24; ++i) v += (0.8f / (float)i) * (i % 3 == 0 ? -1.0f : 0.7f) * ds_sin((float)i * t); break;
            }
            ds_tbl[w][n] = v;
            if (ds_abs(v) > peak) peak = ds_abs(v);
        }
        for (unsigned n = 0; n < DS_TBL; ++n) ds_tbl[w][n] /= peak;
    }
    ds_tbl_ready = 1;
}

static void ds_derive(dubsiren_bank_t *b)
{
    const uint8_t *p = b->pot;
    float u;
    b->tune = 30.0f * ds_exp2(p[SIREN_TUNE] * (1.0f / 127.0f) * 8.2288f);            /* 30 Hz .. 9 kHz */
    b->rate = 0.1f * ds_exp2(p[SIREN_RATE] * (1.0f / 127.0f) * 8.4512f);             /* 0.1 .. 35 Hz */
    b->depth = p[SIREN_DEPTH] * (1.0f / 127.0f);                                      /* 1.0 = +-2 octaves */
    b->cutoff = 100.0f * ds_exp2(p[SIREN_CUTOFF] * (1.0f / 127.0f) * 7.3219f);        /* 100 Hz .. 16 kHz */
    b->k = 1.0f / (0.5f + p[SIREN_RESO] * (1.0f / 127.0f) * 19.5f);                    /* Q 0.5 .. 20 */
    u = (p[SIREN_SWEEP] - 64) * (1.0f / 63.5f);
    b->sweep = ds_abs(u) < 0.03f ? 0.0f : ds_clamp(u, -1.0f, 1.0f);
    b->tunemode = p[SIREN_SWEEPMODE] != 0;
    b->shape = p[SIREN_SHAPE];
    u = p[SIREN_GLIDE] * (1.0f / 127.0f);
    b->glide = 1.0f - ds_exp(-(float)DS_CHUNK / (DS_SR * 0.004f * ds_exp2(u * 10.0f)));   /* time constant 4 ms .. 4 s */
    b->rel = ds_exp(-9.2103f * (float)DS_CHUNK / (DS_SR * (0.1f + 5.9f * p[SIREN_DECAY] * (1.0f / 127.0f))));
    b->level = p[SIREN_VOLUME] * (1.0f / 127.0f) * 0.30f * DS_Q23;
    b->hold = p[SIREN_HOLD] != 0;
}

static void ds_update(dubsiren_bank_t *b)
{
    int want = b->nheld || b->hold;
    if (b->nheld) b->target = (float)b->held[b->nheld - 1] - 60.0f;
    if (want && !b->gate) {
        if (!b->active) {                       /* from silence: start at the pitch, the LFO from its start */
            b->semis = b->target; b->lfo_ph = 0.0f; b->ph = 0.0f; b->env = 0.0f; b->s1 = b->s2 = 0.0f;
        }
        b->active = 1;
    }
    b->gate = (uint8_t)want;
}
void dubsiren_init(dubsiren_bank_t *b)
{
    uint8_t *raw = (uint8_t *)b;
    for (unsigned i = 0; i < sizeof *b; ++i) raw[i] = 0;
    for (unsigned i = 0; i < SIREN_NPARAMS; ++i) b->pot[i] = ds_params[i].def;
    b->rng = 0x1F123BB5u;
    if (!ds_tbl_ready) ds_make_tables();
    ds_derive(b);
}
void dubsiren_panic(dubsiren_bank_t *b) { b->nheld = 0; b->gate = 0; b->active = 0; b->env = 0.0f; b->s1 = b->s2 = 0.0f; }
void dubsiren_set(dubsiren_bank_t *b, unsigned parameter, unsigned value)
{
    if (parameter >= SIREN_NPARAMS) return;
    if (value > ds_params[parameter].max) value = ds_params[parameter].max;
    b->pot[parameter] = (uint8_t)value;
    ds_derive(b);
    if (parameter == SIREN_HOLD) ds_update(b);
}
void dubsiren_note(dubsiren_bank_t *b, unsigned note, unsigned velocity)
{
    if (note > 127) return;
    unsigned i, at = DS_HELD;
    for (i = 0; i < b->nheld; ++i) if (b->held[i] == note) { at = i; break; }
    if (at != DS_HELD) { for (i = at; i + 1 < b->nheld; ++i) b->held[i] = b->held[i + 1]; --b->nheld; }
    if (velocity) {
        if (b->nheld == DS_HELD) { for (i = 0; i + 1 < b->nheld; ++i) b->held[i] = b->held[i + 1]; --b->nheld; }
        b->held[b->nheld++] = (uint8_t)note;
    }
    ds_update(b);
}
void dubsiren_vintage(dubsiren_bank_t *b, unsigned v) { b->vint = (float)(v > 127u ? 127u : v) * (1.0f / 127.0f); }
void dubsiren_bend(dubsiren_bank_t *b, int32_t q16) { b->bend = (float)q16 * (1.0f / 65536.0f); }
void dubsiren_mod(dubsiren_bank_t *b, unsigned v) { b->wheel = (uint8_t)(v > 127 ? 127 : v); }

unsigned dubsiren_render(dubsiren_bank_t *b, int32_t *out)
{
    if (!b->active) return 0;
    static float acc[DUBSIREN_FRAMES];
    float env = b->env;
    for (unsigned c = 0; c < DUBSIREN_FRAMES / DS_CHUNK; ++c) {
        /* the LFO */
        b->lfo_ph += b->rate * (float)DS_CHUNK / DS_SR;
        if (b->lfo_ph >= 1.0f) b->lfo_ph -= 1.0f;
        float p = b->lfo_ph, lfo;
        switch (b->shape) {
        case 0: lfo = ds_sin(p); break;
        case 1: lfo = p < 0.5f ? 1.0f : -1.0f; break;
        case 2: { float q = p + 0.5f; if (q >= 1.0f) q -= 1.0f; lfo = 2.0f * q - 1.0f; break; }
        case 3: lfo = p < 0.25f ? 4.0f * p : p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f; break;
        default: { float x = p * (float)DS_TBL; unsigned i0 = (unsigned)x & (DS_TBL - 1u); float f = x - (float)(int)x;
                   const float *t = ds_tbl[b->shape - 4]; lfo = t[i0] + f * (t[(i0 + 1u) & (DS_TBL - 1u)] - t[i0]); break; }
        }
        /* pitch: keyboard with glide, bend, sweep, the LFO */
        b->semis += (b->target - b->semis) * b->glide;
        float depth = ds_clamp(b->depth + b->wheel * (1.0f / 254.0f), 0.0f, 1.0f);
        float vcut = 0.0f, vpit = 0.0f;
        if (b->vint > 0.0f) {                                          /* vintage: the oscillator and the filter wander a little */
            if (!(b->nch++ & 1023u)) {
                uint32_t n = b->rng; n ^= n << 13; n ^= n >> 17; n ^= n << 5; b->vtd = (float)(n >> 8) * (1.0f / 8388608.0f) - 1.0f;
                n ^= n << 13; n ^= n >> 17; n ^= n << 5; b->vtc = (float)(n >> 8) * (1.0f / 8388608.0f) - 1.0f; b->rng = n;
            }
            b->vd += (b->vtd - b->vd) * 0.004f; b->vc += (b->vtc - b->vc) * 0.004f;
            vpit = b->vd * 0.02f * b->vint; vcut = b->vc * 0.4f * b->vint;     /* octaves: +-24 cents, +-0.4 oct at most */
        }
        float oct = (b->semis + b->bend + (b->tunemode ? b->sweep : 0.0f)) * (1.0f / 12.0f) + lfo * depth * 2.0f + vpit;
        float f = ds_clamp(b->tune * ds_exp2(oct), 20.0f, 12000.0f);
        float inc = f * (1.0f / DS_SR);
        float fc = ds_clamp(b->cutoff * ds_exp2(b->sweep * 1.5849625f + vcut), 40.0f, 16000.0f);   /* x3 per full sweep */
        float g = ds_warp(fc), k = b->k, h = 1.0f / (1.0f + k * g + g * g);
        /* the envelope: 5 ms attack, then the gate or an exponential release */
        float e0 = env;
        if (b->gate) { env += (float)DS_CHUNK / (0.005f * DS_SR); if (env > 1.0f) env = 1.0f; }
        else { env *= b->rel; if (env < 1e-4f) { env = 0.0f; b->active = 0; } }
        float de = (env - e0) * (1.0f / (float)DS_CHUNK);
        float e = e0;
        for (unsigned i = 0; i < DS_CHUNK; ++i) {
            float v = 2.0f * b->ph - 1.0f - ds_blep(b->ph, inc);
            b->ph += inc; if (b->ph >= 1.0f) b->ph -= 1.0f;
            float hp = (v - (k + g) * b->s1 - b->s2) * h;
            float bp = g * hp + b->s1; b->s1 = bp + g * hp;
            float lp = g * bp + b->s2; b->s2 = lp + g * bp;
            e += de;
            acc[c * DS_CHUNK + i] = ds_sat(lp * 0.8f) * e;
        }
        if (!b->active) { for (unsigned i = (c + 1u) * DS_CHUNK; i < DUBSIREN_FRAMES; ++i) acc[i] = 0.0f; break; }
    }
    b->env = env;
    const float lv = b->level * 1.25f;
    for (unsigned i = 0; i < DUBSIREN_FRAMES; ++i) out[i] += (int32_t)ds_clamp(acc[i] * lv, -DS_Q23, DS_Q23 - 1.0f);
    return 1;
}
