/* SPDX-License-Identifier: GPL-3.0-only */
/* polyblep.h - PolyBLEP residual plus libm-free math helpers (C99).
 * floor, abs, exp2, tan and sin are small approximations: the engine needs no libm on the JieLi core. */
#ifndef RC_POLYBLEP_H
#define RC_POLYBLEP_H
#include "../retcon_config.h"

#define RC_PI 3.14159265358979f
#define RC_TWO_PI 6.28318530717959f
#define RC_LOG2E 1.44269504088896f

typedef union { float f; uint32_t u; } rc_bits_t;

static inline float rc_abs(float x) { rc_bits_t b; b.f = x; b.u &= 0x7FFFFFFFu; return b.f; }

/* Bit test, so it also works under -ffast-math. */
static inline int rc_is_finite(float x) { rc_bits_t b; b.f = x; return (b.u & 0x7F800000u) != 0x7F800000u; }
static inline float rc_sanitize(float x) { return rc_is_finite(x) ? x : 0.0f; }

/* Software denormal guard (no flush-to-zero mode to rely on). */
static inline float rc_flush(float x) { return rc_abs(x) < 1.0e-15f ? 0.0f : x; }

static inline float rc_clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static inline float rc_floor(float x)
{
    int32_t i = (int32_t)x;
    return (float)(i - (int32_t)(x < (float)i));
}

/* 2^x, relative error ~1e-5 (about 0.02 cents when used for pitch). */
static inline float rc_exp2(float x)
{
    float fl, f, p;
    int32_t i;
    rc_bits_t s;
    x = rc_clamp(x, -120.0f, 120.0f);
    fl = rc_floor(x);
    f = x - fl;
    i = (int32_t)fl;
    p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f +
        f * (0.00961813f + f * (0.00133336f + f * 0.00015404f)))));
    s.u = (uint32_t)(i + 127) << 23;
    return p * s.f;
}

/* tan(x) for x in [0, ~1.3] (Pade 7/6). The SVF clamps fc so x stays in range. */
static inline float rc_tan(float x)
{
    float x2 = x * x;
    float num = x * (135135.0f - x2 * (17325.0f - x2 * (378.0f - x2)));
    float den = 135135.0f - x2 * (62370.0f - x2 * (3150.0f - x2 * 28.0f));
    return num / den;
}

/* sin(2*pi*phase), phase in [0,1). Parabolic approximation + one refinement, max error ~1e-3. */
static inline float rc_sin_turns(float phase)
{
    float t = phase - 0.5f;                     /* sin(2*pi*t) = -sin(2*pi*phase) */
    float y = 8.0f * t - 16.0f * t * rc_abs(t);
    y = 0.225f * (y * rc_abs(y) - y) + y;
    return -y;
}

/* Spec section D, cubic branch: x - x^3/3 inside [-1,1], +/-2/3 outside. */
static inline float rc_clip3(float x)
{
    if (x > 1.0f) return 2.0f / 3.0f;
    if (x < -1.0f) return -2.0f / 3.0f;
    return x - (x * x * x) * (1.0f / 3.0f);
}

static inline float rc_midi_hz(float note) { return 440.0f * rc_exp2((note - 69.0f) * (1.0f / 12.0f)); }

/* PolyBLEP: t in [0,1) is the oscillator phase, dt the phase increment (f0 / fs). Residual for a
 * discontinuity at t = 0 that is 2 high (peak to peak). */
static inline float rc_blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

#endif
