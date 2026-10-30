/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Tape / BBD echo and spring reverb. See polivoks_fx.h. Float DSP, no libc: runs in the audio
 * interrupt, so code and constants live in internal RAM (see polivoks.c). */
#include "polivoks_fx.h"
#ifndef FM1_HOST_TEST
#endif

#define HALF 22050.0f
#define Q23 8388608.0f
#define BLOCK 128
enum { DLY_N = 16384, DLY_MASK = DLY_N - 1, SPR_MAX = 1024, AP_STAGES = 6 };

const uint8_t polivoks_fx_default[POLIVOKS_FX_PARAMS] = {
    /* time */ 68, /* feed */ 62, /* tone */ 60, /* mix */ 0,
    /* tape */ 0, /* wobble */ 40, /* fx_sat */ 50, /* width */ 40,
    /* decay */ 70, /* tone */ 70, /* drip */ 80, /* mix */ 0};

typedef struct { float z[AP_STAGES]; } ap_chain_t;
typedef struct {
    float buf[SPR_MAX];
    unsigned at, len;
    float lp, gain, lp_a;
    ap_chain_t ap;
} spring_t;

static struct {
    uint8_t pot[POLIVOKS_FX_PARAMS];
    /* echo */
    int16_t buf[DLY_N];
    unsigned wr;
    float len, len_target, len_r_ratio, fb, lp_a, lp1, lp2, hp_x, hp_y, drive;
    float wow_phase, flutter_phase, noise_slow, wobble_depth;
    unsigned bbd;
    float prev_l, prev_r;                 /* last half-rate echo outputs, for upsampling */
    float dly_mix, in_acc, in_prev;
    /* spring */
    spring_t spring[2];
    float ap_coef, in_hp_x, in_hp_y, spr_mix, spr_prev[2], spr_loop_lp;
    float spr_in_acc;
    uint32_t rng;
    unsigned quiet;
} fx;

static inline float fx_clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float fx_absf(float x) { return x < 0 ? -x : x; }
static float fx_exp2f_(float x)
{
    x = fx_clampf(x, -60.0f, 60.0f);
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float a = (x - (float)n) * 0.6931471805599453f;
    float p = 1.0f + a * (1.0f + a * (0.5f + a * (1.0f / 6.0f + a * (1.0f / 24.0f + a * (1.0f / 120.0f + a / 720.0f)))));
    union { uint32_t u; float f; } s;
    s.u = (uint32_t)(n + 127) << 23;
    return p * s.f;
}
static inline float fx_expf_(float x) { return fx_exp2f_(x * 1.4426950408889634f); }
static inline float fx_sat(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
static inline float fx_flush(float x) { return fx_absf(x) < 1e-15f ? 0.0f : x; }
static inline float fx_noise(uint32_t *s)
{
    uint32_t n = *s; n ^= n << 13; n ^= n >> 17; n ^= n << 5; *s = n;
    return (float)(n >> 8) * (1.0f / 8388608.0f) - 1.0f;
}
/* sine from a 0..1 phase: parabola with a correction term, accurate to ~0.1% */
static inline float fx_psin(float p)
{
    p -= (float)(int)p; if (p < 0) p += 1.0f;
    float x = p < 0.5f ? p : p - 1.0f;                  /* -0.5 .. 0.5 */
    float y = 4.0f * x * (1.0f - 2.0f * fx_absf(x));
    return 0.775f * y + 0.225f * y * fx_absf(y);
}
static inline float fx_ap_run(ap_chain_t *c, float x, float a)
{
    for (unsigned i = 0; i < AP_STAGES; ++i) {
        float k = a * (1.0f - 0.07f * (float)i);        /* staggered coefficients spread the dispersion */
        float y = -k * x + c->z[i];
        c->z[i] = fx_flush(x + k * y);
        x = y;
    }
    return x;
}
static inline float fx_lp_coef(float hz) { return 1.0f - fx_expf_(-6.2831853f * hz / HALF); }

/* ---- parameters ---- */
static void fx_derive(void)
{
    const uint8_t *p = fx.pot;
    float u;
    fx.bbd = p[FX_DLY_TYPE] != 0;
    u = p[FX_DLY_TIME] * (1.0f / 127.0f);
    float ms = 35.0f * fx_exp2f_(u * 4.2f) * (fx.bbd ? 0.55f : 1.0f);      /* tape 35..650 ms, BBD 19..357 ms */
    fx.len_target = fx_clampf(ms * 0.001f * HALF, 8.0f, (float)(DLY_N - 16) / 1.5f);
    fx.fb = p[FX_DLY_FEED] * (1.0f / 127.0f) * 1.08f;                    /* slightly over unity: saturation holds it */
    u = p[FX_DLY_TONE] * (1.0f / 127.0f);
    fx.lp_a = fx_lp_coef((fx.bbd ? 700.0f : 1200.0f) * fx_exp2f_(u * (fx.bbd ? 2.8f : 2.9f)));
    fx.drive = 1.0f + 3.0f * (p[FX_DLY_SAT] * (1.0f / 127.0f));
    fx.wobble_depth = p[FX_DLY_WOBBLE] * (1.0f / 127.0f);
    fx.len_r_ratio = 1.0f + 0.5f * (p[FX_DLY_WIDTH] * (1.0f / 127.0f));
    fx.dly_mix = p[FX_DLY_MIX] * (1.0f / 127.0f);
    /* spring: RT60 0.4 .. 6 s, per-spring loop gain from the loop length */
    u = p[FX_SPR_DECAY] * (1.0f / 127.0f);
    float rt60 = 0.4f * fx_exp2f_(u * 3.9f);
    for (unsigned i = 0; i < 2; ++i) {
        fx.spring[i].gain = fx_expf_(-6.9078f * (float)fx.spring[i].len / (rt60 * HALF));
        u = p[FX_SPR_TONE] * (1.0f / 127.0f);
        fx.spring[i].lp_a = fx_lp_coef(1100.0f * fx_exp2f_(u * 2.6f));
    }
    u = p[FX_SPR_DRIP] * (1.0f / 127.0f);
    fx.ap_coef = 0.25f + 0.6f * u;
    fx.spr_mix = p[FX_SPR_MIX] * (1.0f / 127.0f) * 0.7f;
}
void polivoks_fx_set(unsigned parameter, unsigned value)
{
    if (parameter >= POLIVOKS_FX_PARAMS) return;
    if (value > 127) value = 127;
    if (parameter == FX_DLY_TYPE && value > 1) value = 1;
    fx.pot[parameter] = (uint8_t)value;
    fx_derive();
}
void polivoks_fx_reset(void)
{
    uint8_t *raw = (uint8_t *)&fx;
    for (unsigned i = 0; i < sizeof fx; ++i) raw[i] = 0;
    fx.spring[0].len = 655; fx.spring[1].len = 774;        /* ~29.7 ms and ~35.1 ms springs */
    fx.rng = 0x1234567u;
    for (unsigned i = 0; i < POLIVOKS_FX_PARAMS; ++i) fx.pot[i] = polivoks_fx_default[i];
    fx_derive();
    fx.len = fx.len_target;
}

/* ---- processing ---- */
static inline float fx_read_tap(float len)
{
    float rp = (float)fx.wr - len;
    if (rp < 0.0f) rp += (float)DLY_N;
    unsigned i0 = (unsigned)rp & DLY_MASK;
    float f = rp - (float)(int)rp;
    float a = (float)fx.buf[i0], b = (float)fx.buf[(i0 + 1U) & DLY_MASK];
    return (a + f * (b - a)) * (1.0f / 32768.0f);
}
void polivoks_fx_process(const int32_t *in, int32_t *left, int32_t *right)
{
    /* idle: no input for a while and nothing left ringing -> skip the work entirely */
    float peak = 0.0f;
    for (unsigned i = 0; i < BLOCK; ++i) { float a = fx_absf((float)in[i]); if (a > peak) peak = a; }
    if (peak < 40.0f) {
        if (fx.quiet < 100000U) ++fx.quiet;
    } else fx.quiet = 0;
    if (fx.quiet > 3000U && fx.fb < 1.0f && fx.quiet > (unsigned)(8.0f * (fx.len / HALF) / (1.0f - fx.fb * 0.99f) * 344.0f + 600.0f)) return;
    if (fx.dly_mix <= 0.0f && fx.spr_mix <= 0.0f) return;

    const float dmix = fx.dly_mix * 0.9f, smix = fx.spr_mix;
    for (unsigned n = 0; n < BLOCK; n += 2) {
        float x = 0.5f * ((float)in[n] + (float)in[n + 1]) * (1.0f / Q23);

        /* ---- echo, one half-rate step ---- */
        fx.len += (fx.len_target - fx.len) * 0.0006f;                       /* tape-style pitch bend when Time moves */
        fx.wow_phase += (fx.bbd ? 0.35f : 0.62f) / HALF;
        fx.flutter_phase += (fx.bbd ? 1.9f : 7.3f) / HALF;
        fx.noise_slow += (fx_noise(&fx.rng) - fx.noise_slow) * 0.002f;
        float mod = fx.bbd ? 0.0007f * HALF * fx_psin(fx.wow_phase) + 0.00006f * HALF * fx.noise_slow * 6.0f
                           : 0.0012f * HALF * fx_psin(fx.wow_phase) + 0.00010f * HALF * fx_psin(fx.flutter_phase)
                             + 0.00008f * HALF * fx.noise_slow * 6.0f;
        mod *= fx.wobble_depth;
        float tl = fx_read_tap(fx.len + mod);
        float tr = fx_read_tap(fx_clampf(fx.len * fx.len_r_ratio + mod, 4.0f, (float)(DLY_N - 4)));
        /* feedback: darken (1 or 2 poles), saturate, block DC */
        fx.lp1 += (tl - fx.lp1) * fx.lp_a; fx.lp1 = fx_flush(fx.lp1);
        float fbs = fx.lp1;
        if (fx.bbd) { fx.lp2 += (fbs - fx.lp2) * fx.lp_a; fx.lp2 = fx_flush(fx.lp2); fbs = fx.lp2; }
        float loop = fx_sat(fbs * fx.drive) * (1.0f / fx.drive) * fx.fb;
        float dc = loop - fx.hp_x + 0.995f * fx.hp_y; fx.hp_x = loop; fx.hp_y = fx_flush(dc); loop = fx.hp_y;
        float w = x + loop;
        fx.buf[fx.wr] = (int16_t)(fx_clampf(w, -0.999f, 0.999f) * 32767.0f);
        fx.wr = (fx.wr + 1U) & DLY_MASK;

        /* ---- spring: fed by the dry signal plus the echoes ---- */
        float sin_ = x + 0.7f * tl;
        float hp = sin_ - fx.in_hp_x + 0.985f * fx.in_hp_y; fx.in_hp_x = sin_; fx.in_hp_y = fx_flush(hp);
        float drive_in = fx_sat(fx.in_hp_y * 2.2f) * 0.45f;
        float sp[2];
        for (unsigned s = 0; s < 2; ++s) {
            spring_t *sps = &fx.spring[s];
            unsigned rd = sps->at;                                           /* oldest sample: written len steps ago */
            float out = sps->buf[rd];
            sps->lp += (out - sps->lp) * sps->lp_a; sps->lp = fx_flush(sps->lp);
            float fed = fx_ap_run(&sps->ap, sps->lp, fx.ap_coef) * sps->gain;
            float v = fx_flush(drive_in * (s ? 0.9f : 1.0f) + fed);
            sps->buf[rd] = fx_clampf(v, -4.0f, 4.0f);
            sps->at = rd + 1U >= sps->len ? 0U : rd + 1U;
            sp[s] = out;
        }
        float spl = sp[0] + 0.35f * sp[1], spr = sp[1] + 0.35f * sp[0];

        /* ---- upsample (linear) and add to the output ---- */
        float wl = dmix * tl + smix * spl, wr_ = dmix * tr + smix * spr;
        float l0 = 0.5f * (fx.prev_l + wl), r0 = 0.5f * (fx.prev_r + wr_);
        fx.prev_l = wl; fx.prev_r = wr_;
        float *o[4] = {&l0, &wl, &r0, &wr_};
        int32_t add[4];
        for (unsigned k = 0; k < 4; ++k) add[k] = (int32_t)(fx_clampf(*o[k], -1.0f, 1.0f) * (Q23 - 1.0f));
        left[n] += add[0]; left[n + 1] += add[1];
        right[n] += add[2]; right[n + 1] += add[3];
    }
}
