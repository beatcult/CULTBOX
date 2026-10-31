/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* BBD echo. See polivoks_fx.h. Float DSP, no libc: runs in the audio interrupt. */
#include "polivoks_fx.h"

#define HALF 22050.0f
#define Q23 8388608.0f
#define BLOCK 128
enum { DLY_N = 32768, DLY_MASK = DLY_N - 1 };      /* 1.49 s at the half rate */
static int16_t fx_buf[DLY_N] __attribute__((section(".pool")));   /* the pool: big buffers, zeroed at boot */

const uint8_t polivoks_fx_default[POLIVOKS_FX_PARAMS] = {/* time */ 48, /* feed */ 62, /* tone */ 60, /* mix */ 0};

static struct {
    uint8_t pot[POLIVOKS_FX_PARAMS];
    unsigned wr;
    float len, len_target, fb, lp_a, lp1, lp2, hp_x, hp_y, mix;
    float wow_phase, noise_slow, prev_l, prev_r;
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
static inline float fx_psin(float p)               /* sine from a 0..1 phase, ~0.1 % */
{
    p -= (float)(int)p; if (p < 0) p += 1.0f;
    float x = p < 0.5f ? p : p - 1.0f;
    float y = 4.0f * x * (1.0f - 2.0f * fx_absf(x));
    return 0.775f * y + 0.225f * y * fx_absf(y);
}

static void fx_derive(void)
{
    const uint8_t *p = fx.pot;
    float u = p[FX_DLY_TIME] * (1.0f / 127.0f);
    float ms = 19.0f * fx_exp2f_(u * 5.9f);        /* 19 ms .. 1.1 s */
    fx.len_target = fx_clampf(ms * 0.001f * HALF, 8.0f, (float)(DLY_N - 16) / 1.5f);
    fx.fb = p[FX_DLY_FEED] * (1.0f / 127.0f) * 1.08f;   /* a little over unity: the saturation holds it */
    u = p[FX_DLY_TONE] * (1.0f / 127.0f);
    fx.lp_a = 1.0f - fx_expf_(-6.2831853f * (700.0f * fx_exp2f_(u * 2.8f)) / HALF);
    fx.mix = p[FX_DLY_MIX] * (1.0f / 127.0f) * 0.9f;
}
void polivoks_fx_set(unsigned parameter, unsigned value)
{
    if (parameter >= POLIVOKS_FX_PARAMS) return;
    if (value > 127) value = 127;
    fx.pot[parameter] = (uint8_t)value;
    fx_derive();
}
void polivoks_fx_reset(void)
{
    uint8_t *raw = (uint8_t *)&fx;
    for (unsigned i = 0; i < sizeof fx; ++i) raw[i] = 0;
    for (unsigned i = 0; i < DLY_N; ++i) fx_buf[i] = 0;
    fx.rng = 0x1234567u;
    for (unsigned i = 0; i < POLIVOKS_FX_PARAMS; ++i) fx.pot[i] = polivoks_fx_default[i];
    fx_derive();
    fx.len = fx.len_target;
}

static inline float fx_tap(float len)
{
    float rp = (float)fx.wr - len;
    if (rp < 0.0f) rp += (float)DLY_N;
    unsigned i0 = (unsigned)rp & DLY_MASK;
    float f = rp - (float)(int)rp;
    float a = (float)fx_buf[i0], b = (float)fx_buf[(i0 + 1U) & DLY_MASK];
    return (a + f * (b - a)) * (1.0f / 32768.0f);
}
void polivoks_fx_process(const int32_t *in, int32_t *left, int32_t *right)
{
    float peak = 0.0f;
    if (fx.mix <= 0.0f) return;
    for (unsigned i = 0; i < BLOCK; ++i) { float a = fx_absf((float)in[i]); if (a > peak) peak = a; }
    if (peak < 40.0f) { if (fx.quiet < 100000U) ++fx.quiet; } else fx.quiet = 0;
    /* idle: silent input for long enough that every repeat has died away -> skip */
    if (fx.quiet > 3000U && fx.fb < 1.0f &&
        fx.quiet > (unsigned)(8.0f * (fx.len / HALF) / (1.0f - fx.fb * 0.99f) * 344.0f + 600.0f)) return;

    const float mix = fx.mix;
    for (unsigned n = 0; n < BLOCK; n += 2) {
        float x = 0.5f * ((float)in[n] + (float)in[n + 1]) * (1.0f / Q23);
        fx.len += (fx.len_target - fx.len) * 0.0006f;           /* a bend when Time moves, as a clock change does */
        fx.wow_phase += 0.35f / HALF;
        fx.noise_slow += (fx_noise(&fx.rng) - fx.noise_slow) * 0.002f;
        float mod = 0.00035f * HALF * fx_psin(fx.wow_phase) + 0.0002f * HALF * fx.noise_slow;
        float tl = fx_tap(fx.len + mod);
        float tr = fx_tap(fx_clampf(fx.len * 1.2f + mod, 4.0f, (float)(DLY_N - 4)));
        fx.lp1 += (tl - fx.lp1) * fx.lp_a; fx.lp1 = fx_flush(fx.lp1);       /* two poles: the BBD's dark repeats */
        fx.lp2 += (fx.lp1 - fx.lp2) * fx.lp_a; fx.lp2 = fx_flush(fx.lp2);
        float loop = fx_sat(fx.lp2 * 2.2f) * (1.0f / 2.2f) * fx.fb;
        float dc = loop - fx.hp_x + 0.995f * fx.hp_y; fx.hp_x = loop; fx.hp_y = fx_flush(dc); loop = fx.hp_y;
        fx_buf[fx.wr] = (int16_t)(fx_clampf(x + loop, -0.999f, 0.999f) * 32767.0f);
        fx.wr = (fx.wr + 1U) & DLY_MASK;

        float wl = mix * tl, wr = mix * tr;                       /* up to full rate, linear */
        float l0 = 0.5f * (fx.prev_l + wl), r0 = 0.5f * (fx.prev_r + wr);
        fx.prev_l = wl; fx.prev_r = wr;
        left[n] += (int32_t)(fx_clampf(l0, -1.0f, 1.0f) * (Q23 - 1.0f));
        left[n + 1] += (int32_t)(fx_clampf(wl, -1.0f, 1.0f) * (Q23 - 1.0f));
        right[n] += (int32_t)(fx_clampf(r0, -1.0f, 1.0f) * (Q23 - 1.0f));
        right[n + 1] += (int32_t)(fx_clampf(wr, -1.0f, 1.0f) * (Q23 - 1.0f));
    }
}
