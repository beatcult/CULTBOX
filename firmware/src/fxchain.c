/* SPDX-License-Identifier: GPL-3.0-only */
/* Per-channel effects chain. See fxchain.h. No libm. */
#include "fxchain.h"

#define FC_SR 44100.0f
#define FC_Q23 8388608.0f
#define FC_N 256u                       /* tape delay line (a power of two) */
#define FC_CENTER 128.0f                /* its rest delay, samples: the wow and flutter swing either side */
#define FC_CHUNK 16u
#define FC_MAXF 128u

typedef struct {
    uint8_t p[FXC_PARAMS];
    /* saturation */
    float sd, sg, smix, xp, dcx, dcy;
    /* tape */
    float tg, tgc, tmix, wd, fd, hf_a, hiss_t;
    float tl[FC_N];
    uint32_t w, nch;
    float dcur, wph, fph, fph2, wn, fn, bump_lp, lp1, lp2, bump_g, nprev;
    /* vintage */
    float va, vlp;
    uint32_t rng;
} fxc_t;
static fxc_t fcs[FXC_CHANNELS];
static float fc_buf[FC_MAXF];

static inline float fc_abs(float x) { return x < 0 ? -x : x; }
static float fc_exp2(float x)
{
    x = x < -60.0f ? -60.0f : x > 60.0f ? 60.0f : x;
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float a = (x - (float)n) * 0.6931471805599453f;
    float p = 1.0f + a * (1.0f + a * (0.5f + a * (1.0f / 6.0f + a * (1.0f / 24.0f + a * (1.0f / 120.0f + a / 720.0f)))));
    union { uint32_t u; float f; } s;
    s.u = (uint32_t)(n + 127) << 23;
    return p * s.f;
}
static inline float fc_exp(float x) { return fc_exp2(x * 1.4426950408889634f); }
static float fc_sin(float turns)                /* sine of 2*pi*turns, ~0.1 % */
{
    turns -= (float)(int)turns; if (turns < 0) turns += 1.0f;
    float x = turns < 0.5f ? turns : turns - 1.0f;
    float y = 8.0f * x * (1.0f - 2.0f * fc_abs(x));
    return 0.775f * y + 0.225f * y * fc_abs(y);
}
static inline float fc_noise(uint32_t *s)
{
    uint32_t n = *s; n ^= n << 13; n ^= n >> 17; n ^= n << 5; *s = n;
    return (float)(n >> 8) * (1.0f / 8388608.0f) - 1.0f;
}
static inline float fc_tanh(float x)            /* a smooth saturator, +-1 */
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

/* the five models, u = the driven input (1.0 = full scale) */
static float fc_shape(unsigned m, float u)
{
    switch (m) {
    case FXC_TUBE: return fc_tanh(u + 0.3f) - 0.2915f;                                   /* biased: even harmonics, warm */
    case FXC_DIODE: return u >= 0.0f ? fc_tanh(u) : 0.6f * fc_tanh(u * (1.0f / 0.6f));   /* one side clips earlier */
    case FXC_FUZZ: {                                                                      /* a hard, square shoulder */
        float v = u > 1.0f ? 1.0f : u < -1.0f ? -1.0f : u;
        return v * (1.5f - 0.5f * v * v); }
    case FXC_FOLD: {                                                                      /* a wavefolder: past full scale it turns round */
        float t, f;
        u = u > 60.0f ? 60.0f : u < -60.0f ? -60.0f : u;
        t = u * 0.25f + 0.75f + 64.0f;
        f = t - (float)(int)t;
        return fc_abs(f * 4.0f - 2.0f) - 1.0f; }
    default: {                                                                            /* rectified: an octave up under the note */
        float s = fc_tanh(u);
        return 0.6f * s + 0.9f * fc_abs(s) - 0.45f; }
    }
}

void fxc_reset(void)
{
    unsigned c, i;
    for (c = 0; c < FXC_CHANNELS; c++) {
        fxc_t *f = &fcs[c];
        uint8_t *raw = (uint8_t *)f;
        for (i = 0; i < sizeof *f; i++) raw[i] = 0;
        f->rng = 0x9E3779B9u + c * 0x01000193u;
        f->tg = 1.0f; f->tgc = 1.0f; f->sd = 1.0f; f->sg = 1.0f; f->dcur = FC_CENTER;
        f->hf_a = 1.0f; f->va = 1.0f; f->wph = 0.1f * (float)c; f->fph = 0.3f * (float)c;
        f->p[FXC_DRIVE] = 64; f->p[FXC_TDRIVE] = 64; f->p[FXC_WOW] = 40; f->p[FXC_AGE] = 40;
    }
}
void fxc_set(unsigned c, unsigned p, unsigned v)
{
    fxc_t *f;
    float u;
    if (c >= FXC_CHANNELS || p >= FXC_PARAMS) return;
    f = &fcs[c];
    if (v > 127u) v = 127u;
    f->p[p] = (uint8_t)v;
    u = (float)v * (1.0f / 127.0f);
    switch (p) {
    case FXC_VINT: {
        float fcut = 18000.0f * (1.0f - 0.66f * u);                                       /* a little dark: 18 kHz .. 6 kHz */
        f->va = 1.0f - fc_exp(-6.2831853f * fcut / FC_SR);
        break; }
    case FXC_DRIVE: {
        float k = 24.0f * u * u * 0.5f;                                                   /* level kept: full at no drive, -7 dB from drive 3 up */
        f->sd = 1.0f + 24.0f * u * u;
        f->sg = 1.0f - 0.55f * (k > 1.0f ? 1.0f : k);
        break; }
    case FXC_SMIX: f->smix = u; break;
    case FXC_TDRIVE: f->tg = 1.0f + 3.0f * u; f->tgc = 1.0f / (0.4f + 0.6f * f->tg); break;
    case FXC_WOW: f->wd = 110.0f * u * u; f->fd = 9.0f * u * u; break;
    case FXC_AGE: {
        float fcut = 16000.0f * (1.0f - 0.75f * u);                                       /* tape loss: 16 kHz .. 4 kHz */
        f->hf_a = 1.0f - fc_exp(-6.2831853f * fcut / FC_SR);
        f->hiss_t = 0.0015f * u;
        f->bump_g = 0.15f + 0.3f * u;
        break; }
    case FXC_TMIX: f->tmix = u; break;
    default: break;
    }
    if (p == FXC_TDRIVE || p == FXC_AGE) f->bump_g = (0.15f + 0.3f * (float)f->p[FXC_AGE] * (1.0f / 127.0f)) * (0.4f + 0.6f * (float)f->p[FXC_TDRIVE] * (1.0f / 127.0f));
}
unsigned fxc_active(unsigned c)
{
    return c < FXC_CHANNELS && (fcs[c].p[FXC_VINT] || fcs[c].p[FXC_SMIX] || fcs[c].p[FXC_TMIX]);
}

void fxc_process(unsigned c, int32_t *buf, unsigned n)
{
    fxc_t *f;
    float *x = fc_buf;
    unsigned i, m;
    if (c >= FXC_CHANNELS || n > FC_MAXF || !fxc_active(c)) return;
    f = &fcs[c];
    for (i = 0; i < n; i++) x[i] = (float)buf[i] * (1.0f / FC_Q23);

    if (f->p[FXC_SMIX]) {                                  /* saturation: the model, 2x oversampled, no DC, level kept */
        const unsigned model = f->p[FXC_MODEL] < FXC_MODELS ? f->p[FXC_MODEL] : 0u;
        const float sd = f->sd, sg = f->sg, mx = f->smix;
        float xp = f->xp, dcx = f->dcx, dcy = f->dcy;
        for (i = 0; i < n; i++) {
            float d = x[i], mid = 0.5f * (xp + d), y, z;
            y = 0.5f * (fc_shape(model, mid * sd) + fc_shape(model, d * sd)) * sg;
            xp = d;
            z = y - dcx + 0.995f * dcy; dcx = y; dcy = z;
            x[i] = d + (z - d) * mx;
        }
        f->xp = xp; f->dcx = dcx; f->dcy = dcy;
    }

    if (f->p[FXC_TMIX]) {                                  /* tape */
        const float tg = f->tg, tgc = f->tgc, mx = f->tmix, ha = f->hf_a, bg = f->bump_g, hs = f->hiss_t;
        const float ba = 0.0127f;                          /* the head bump: a low shelf near 90 Hz */
        float lp1 = f->lp1, lp2 = f->lp2, bl = f->bump_lp, np = f->nprev;
        uint32_t w = f->w;
        float *tl = f->tl;
        for (m = 0; m < n; m += FC_CHUNK) {
            float tgt, dd;
            unsigned e = m + FC_CHUNK < n ? m + FC_CHUNK : n;
            f->wph += FC_CHUNK * 0.45f / FC_SR; if (f->wph >= 1.0f) f->wph -= 1.0f;
            f->fph += FC_CHUNK * 6.2f / FC_SR; if (f->fph >= 1.0f) f->fph -= 1.0f;
            f->fph2 += FC_CHUNK * 10.3f / FC_SR; if (f->fph2 >= 1.0f) f->fph2 -= 1.0f;
            if (!(f->nch++ % 600u)) f->fn = fc_noise(&f->rng);                           /* a new place for the speed to wander to, a few times a second */
            f->wn += (f->fn - f->wn) * 0.0008f;
            tgt = FC_CENTER + f->wd * (0.6f * fc_sin(f->wph) + 0.35f * f->wn) + f->fd * (0.5f * fc_sin(f->fph) + 0.3f * fc_sin(f->fph2));
            tgt = tgt < 8.0f ? 8.0f : tgt > 240.0f ? 240.0f : tgt;
            dd = (tgt - f->dcur) * (1.0f / (float)FC_CHUNK);
            for (i = m; i < e; i++) {
                float s = x[i], y, dly, fr, a, b, dry;
                unsigned k;
                s = fc_tanh(s * tg) * tgc;                                    /* tape saturation: soft, level kept */
                bl += ba * (s - bl);
                s += bg * bl;                                                  /* head bump */
                lp1 += ha * (s - lp1); lp2 += ha * (lp1 - lp2); s = lp2;     /* loss */
                tl[w & (FC_N - 1u)] = s;
                f->dcur += dd;
                dly = f->dcur; k = (unsigned)dly; fr = dly - (float)k;
                a = tl[(w - k) & (FC_N - 1u)]; b = tl[(w - k - 1u) & (FC_N - 1u)];
                y = a + (b - a) * fr;
                dry = tl[(w - (unsigned)FC_CENTER) & (FC_N - 1u)];
                {
                    float nz = fc_noise(&f->rng);
                    y += hs * (nz - np); np = nz;                              /* hiss, weighted to the top */
                }
                w++;
                x[i] = dry + (y - dry) * mx;
            }
        }
        f->w = w; f->lp1 = lp1; f->lp2 = lp2; f->bump_lp = bl; f->nprev = np;
    }

    if (f->p[FXC_VINT]) {                                  /* vintage: a little darker (no hiss: it wears thin) */
        const float a = f->va;
        float lp = f->vlp;
        for (i = 0; i < n; i++) {
            lp += a * (x[i] - lp);
            x[i] = lp;
        }
        f->vlp = lp;
    }

    for (i = 0; i < n; i++) {
        float s = x[i] * FC_Q23;
        buf[i] = (int32_t)(s > FC_Q23 - 1.0f ? FC_Q23 - 1.0f : s < -FC_Q23 ? -FC_Q23 : s);
    }
}
