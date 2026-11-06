/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* BBD echo. See polivoks_fx.h. Float DSP, no libc: runs in the audio interrupt.
 * The echo itself is polivoks_fx_t (its own buffer, its own sample rate); the FM-1 keeps two of them behind the old global calls. */
#include "polivoks_fx.h"

#define HALF_FM1 22050.0f                 /* the FM-1: 44.1 kHz, the echo at half of it */
#define Q23 8388608.0f
#define BLOCK 128
enum { DLY_N = 32768 };                           /* 1.49 s at the FM-1's half rate */
#ifndef POLIVOKS_FX_NO_LEGACY      /* (a plugin builds with this: its echoes are its own, and there is no .pool) */
static int16_t fx_buf[POLIVOKS_FX_UNITS][DLY_N] __attribute__((section(".pool")));   /* the pool: big buffers, zeroed at boot */
#endif

const uint8_t polivoks_fx_default[POLIVOKS_FX_PARAMS] = {/* time */ 48, /* feed */ 62, /* tone */ 60, /* mix */ 0};

#ifndef POLIVOKS_FX_NO_LEGACY
static polivoks_fx_t fxs[POLIVOKS_FX_UNITS];
#endif
/* a note value in beats: 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2 */
static const float NOTE_BEATS[POLIVOKS_FX_NOTES] = {0.125f, 0.16666667f, 0.25f, 0.33333333f, 0.375f, 0.5f, 0.66666667f, 0.75f, 1.0f, 1.33333333f, 1.5f, 2.0f};

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

static void fx_len(polivoks_fx_t *f)                        /* the delay, free (a time) or from the tempo (a note value, halved until the buffer holds it) */
{
    const float max = (float)(f->n - 16u) / 1.5f;
    if (f->sync && f->beat > 0.0f) {
        float t = f->beat * NOTE_BEATS[f->note];
        for (int n = 0; n < 6 && t > max; ++n) t *= 0.5f;
        f->len_target = fx_clampf(t, 8.0f, max);
    } else {
        float u = f->pot[FX_DLY_TIME] * (1.0f / 127.0f);
        float ms = 19.0f * fx_exp2f_(u * 5.9f);        /* 19 ms .. 1.1 s */
        f->len_target = fx_clampf(ms * 0.001f * f->half, 8.0f, max);
    }
}
static void fx_derive(polivoks_fx_t *f)
{
    const uint8_t *p = f->pot;
    float u;
    fx_len(f);
    f->fb = p[FX_DLY_FEED] * (1.0f / 127.0f) * 1.08f;   /* a little over unity: the saturation holds it */
    u = p[FX_DLY_TONE] * (1.0f / 127.0f);
    f->lp_a = 1.0f - fx_expf_(-6.2831853f * (700.0f * fx_exp2f_(u * 2.8f)) / f->half);
    f->mix = p[FX_DLY_MIX] * (1.0f / 127.0f) * 0.9f;
}
unsigned polivoks_fx_buffer_len(float sample_rate)       /* samples (int16) of delay line a unit needs: a power of two; 32768 at 44.1 kHz and below */
{
    unsigned n = DLY_N;
    float need = 0.99f * (sample_rate * 0.5f) * 1.5f + 16.0f;      /* the longest Time, and the right tap's 1.2 x */
    while ((float)n < need && n < (1u << 20)) n <<= 1;
    return n;
}
void polivoks_fx_unit_init(polivoks_fx_t *f, int16_t *buf, unsigned n, float sample_rate, unsigned seed)
{
    uint8_t *raw = (uint8_t *)f;
    for (unsigned i = 0; i < sizeof *f; ++i) raw[i] = 0;
    f->buf = buf; f->n = n; f->mask = n - 1u;
    f->half = sample_rate * 0.5f;
    f->rate = f->half * (1.0f / HALF_FM1);              /* 1 on the FM-1 */
    for (unsigned i = 0; i < n; ++i) buf[i] = 0;
    f->rng = 0x1234567u + seed * 7919u;
    for (unsigned i = 0; i < POLIVOKS_FX_PARAMS; ++i) f->pot[i] = polivoks_fx_default[i];
    fx_derive(f);
    f->len = f->len_target;
}
void polivoks_fx_unit_set(polivoks_fx_t *f, unsigned parameter, unsigned value)
{
    if (parameter >= POLIVOKS_FX_PARAMS) return;
    if (value > 127) value = 127;
    f->pot[parameter] = (uint8_t)value;
    fx_derive(f);
}
void polivoks_fx_unit_sync(polivoks_fx_t *f, unsigned on, unsigned note)
{
    f->sync = (uint8_t)(on ? 1 : 0);
    f->note = (uint8_t)(note >= POLIVOKS_FX_NOTES ? POLIVOKS_FX_NOTES - 1u : note);
    fx_len(f);
}
void polivoks_fx_unit_tempo(polivoks_fx_t *f, float samples_per_beat)
{
    f->beat = samples_per_beat * 0.5f;                  /* (the echo runs at half rate) */
    if (f->sync) fx_len(f);
}

#ifndef POLIVOKS_FX_NO_LEGACY
/* the FM-1's two echoes, behind the old global calls */
void polivoks_fx_set(unsigned unit, unsigned parameter, unsigned value)
{
    if (unit >= POLIVOKS_FX_UNITS) return;
    polivoks_fx_unit_set(&fxs[unit], parameter, value);
}
void polivoks_fx_sync(unsigned unit, unsigned on, unsigned note)
{
    if (unit >= POLIVOKS_FX_UNITS) return;
    polivoks_fx_unit_sync(&fxs[unit], on, note);
}
void polivoks_fx_tempo(float samples_per_beat)
{
    for (unsigned u = 0; u < POLIVOKS_FX_UNITS; ++u) polivoks_fx_unit_tempo(&fxs[u], samples_per_beat);
}
void polivoks_fx_reset(void)
{
    for (unsigned u = 0; u < POLIVOKS_FX_UNITS; ++u) polivoks_fx_unit_init(&fxs[u], fx_buf[u], DLY_N, 2.0f * HALF_FM1, u);
}
#endif

static inline float fx_tap(const polivoks_fx_t *u, float len)
{
    const int16_t *buf = u->buf;
    float rp = (float)u->wr - len;
    if (rp < 0.0f) rp += (float)u->n;
    unsigned i0 = (unsigned)rp & u->mask;
    float f = rp - (float)(int)rp;
    float a = (float)buf[i0], b = (float)buf[(i0 + 1U) & u->mask];
    return (a + f * (b - a)) * (1.0f / 32768.0f);
}
unsigned polivoks_fx_unit_process(polivoks_fx_t *f, const int32_t *in, int32_t *left, int32_t *right)
{
    float peak = 0.0f;
    int16_t *buf = f->buf;
    if (f->mix <= 0.0f) return 0;
    for (unsigned i = 0; i < BLOCK; ++i) { float a = fx_absf((float)in[i]); if (a > peak) peak = a; }
    if (peak < 40.0f) { if (f->quiet < 100000U) ++f->quiet; } else f->quiet = 0;
    /* idle: silent input for long enough that every repeat has died away -> skip */
    if (f->quiet > 3000U && f->fb < 1.0f &&
        f->quiet > (unsigned)(8.0f * (f->len / f->half) / (1.0f - f->fb * 0.99f) * (344.0f * f->rate) + 600.0f)) return 0;

    const float mix = f->mix;
    for (unsigned n = 0; n < BLOCK; n += 2) {
        float x = 0.5f * ((float)in[n] + (float)in[n + 1]) * (1.0f / Q23);
        f->len += (f->len_target - f->len) * 0.0006f;           /* a bend when Time moves, as a clock change does */
        f->wow_phase += 0.35f / f->half;
        f->noise_slow += (fx_noise(&f->rng) - f->noise_slow) * 0.002f;
        float mod = 0.00035f * f->half * fx_psin(f->wow_phase) + 0.0002f * f->half * f->noise_slow;
        float tl = fx_tap(f, f->len + mod);
        float tr = fx_tap(f, fx_clampf(f->len * 1.2f + mod, 4.0f, (float)(f->n - 4u)));
        f->lp1 += (tl - f->lp1) * f->lp_a; f->lp1 = fx_flush(f->lp1);       /* two poles: the BBD's dark repeats */
        f->lp2 += (f->lp1 - f->lp2) * f->lp_a; f->lp2 = fx_flush(f->lp2);
        float loop = fx_sat(f->lp2 * 2.2f) * (1.0f / 2.2f) * f->fb;
        float dc = loop - f->hp_x + 0.995f * f->hp_y; f->hp_x = loop; f->hp_y = fx_flush(dc); loop = f->hp_y;
        buf[f->wr] = (int16_t)(fx_clampf(x + loop, -0.999f, 0.999f) * 32767.0f);
        f->wr = (f->wr + 1U) & f->mask;

        float wl = mix * tl, wr = mix * tr;                       /* up to full rate, linear */
        float l0 = 0.5f * (f->prev_l + wl), r0 = 0.5f * (f->prev_r + wr);
        f->prev_l = wl; f->prev_r = wr;
        left[n] += (int32_t)(fx_clampf(l0, -1.0f, 1.0f) * (Q23 - 1.0f));
        left[n + 1] += (int32_t)(fx_clampf(wl, -1.0f, 1.0f) * (Q23 - 1.0f));
        right[n] += (int32_t)(fx_clampf(r0, -1.0f, 1.0f) * (Q23 - 1.0f));
        right[n + 1] += (int32_t)(fx_clampf(wr, -1.0f, 1.0f) * (Q23 - 1.0f));
    }
    return 1;
}
#ifndef POLIVOKS_FX_NO_LEGACY
unsigned polivoks_fx_process(unsigned unit, const int32_t *in, int32_t *left, int32_t *right)
{
    if (unit >= POLIVOKS_FX_UNITS) return 0;
    return polivoks_fx_unit_process(&fxs[unit], in, left, right);
}
#endif
