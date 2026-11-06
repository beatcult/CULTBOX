/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin core at other sample rates: PLVKS keeps its pitch and its times, the echo keeps its delay in milliseconds,
 * and two echoes share nothing.   cc -O1 -o core_rate_test tests/core_rate_test.c -lm && ./core_rate_test */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../firmware/src/polivoks.c"
#include "../firmware/src/polivoks_fx.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const float RATES[] = {44100.0f, 48000.0f, 88200.0f, 96000.0f};

/* the pitch of a held note, from the zero crossings of a plain triangle through a wide-open filter */
static double pitch_hz(float sr, unsigned note)
{
    uint32_t storage[POLIVOKS_BANK_BYTES / 4];
    polivoks_bank_t *b = (polivoks_bank_t *)storage;
    int32_t out[POLIVOKS_FRAMES];
    polivoks_bank_init(b); polivoks_bank_set_rate(b, sr);
    polivoks_bank_set(b, POLIVOKS_CUTOFF, 127); polivoks_bank_set(b, POLIVOKS_RESO, 0); polivoks_bank_set(b, POLIVOKS_ENVAMT, 0);
    polivoks_bank_set(b, POLIVOKS_WAVE1, POLIVOKS_WAVE_TRI); polivoks_bank_set(b, POLIVOKS_LEVEL2, 0);
    polivoks_bank_set(b, POLIVOKS_DRIVE, 0); polivoks_bank_set(b, POLIVOKS_VOICES, POLIVOKS_MONO);
    polivoks_bank_note(b, note, 100, 0);
    long n = 0, zc = 0; double prev = 0;
    int blocks = (int)(sr * 1.5f / POLIVOKS_FRAMES);
    for (int k = 0; k < blocks; ++k) {
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) out[i] = 0;
        polivoks_bank_render(b, out);
        if (k < blocks / 3) continue;                    /* the attack */
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) { double v = out[i]; if ((v >= 0) != (prev >= 0)) ++zc; prev = v; ++n; }
    }
    return zc / 2.0 / (n / (double)sr);
}

/* the time a released note takes to fall 34 dB (to 2 %), in ms */
static double release_ms(float sr)
{
    uint32_t storage[POLIVOKS_BANK_BYTES / 4];
    polivoks_bank_t *b = (polivoks_bank_t *)storage;
    int32_t out[POLIVOKS_FRAMES];
    polivoks_bank_init(b); polivoks_bank_set_rate(b, sr);
    polivoks_bank_set(b, POLIVOKS_RELEASE, 70); polivoks_bank_set(b, POLIVOKS_SUSTAIN, 127);
    polivoks_bank_note(b, 60, 100, 0);
    double pk = 0;
    for (int k = 0; k < (int)(sr * 1.0f / POLIVOKS_FRAMES); ++k) {
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) out[i] = 0;
        polivoks_bank_render(b, out);
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) if (fabs((double)out[i]) > pk) pk = fabs((double)out[i]);
    }
    polivoks_bank_note(b, 60, 0, 0);
    long t = 0;
    const int win = (int)(sr * 0.025f / POLIVOKS_FRAMES) + 1;      /* 25 ms windows: longer than a period of the note, whatever the block */
    for (int w = 0; w < (int)(sr * 20.0f / POLIVOKS_FRAMES / win); ++w) {
        double wpk = 0;
        int alive = 1;
        for (int k = 0; k < win && alive; ++k) {
            for (int i = 0; i < POLIVOKS_FRAMES; ++i) out[i] = 0;
            alive = (int)polivoks_bank_render(b, out);
            for (int i = 0; i < POLIVOKS_FRAMES; ++i) if (fabs((double)out[i]) > wpk) wpk = fabs((double)out[i]);
            t += POLIVOKS_FRAMES;
        }
        if (!alive || wpk < pk * 0.02) break;
    }
    return t * 1000.0 / sr;
}

/* an impulse through the echo: the time of the first repeat, in ms */
static double echo_ms(float sr, unsigned time_pot, int16_t **bufp)
{
    polivoks_fx_t f;
    unsigned n = polivoks_fx_buffer_len(sr);
    int16_t *buf = (int16_t *)calloc(n, sizeof *buf);
    int32_t in[POLIVOKS_FRAMES], l[POLIVOKS_FRAMES], r[POLIVOKS_FRAMES];
    polivoks_fx_unit_init(&f, buf, n, sr, 0);
    polivoks_fx_unit_set(&f, FX_DLY_TIME, time_pot); polivoks_fx_unit_set(&f, FX_DLY_FEED, 0);
    polivoks_fx_unit_set(&f, FX_DLY_TONE, 127); polivoks_fx_unit_set(&f, FX_DLY_MIX, 127);
    long pos = 0, best = -1; double bestv = 0;
    for (int k = 0; k < (int)(sr * 1.4f / POLIVOKS_FRAMES); ++k) {
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) in[i] = l[i] = r[i] = 0;
        if (k == 0) in[0] = in[1] = 8000000;           /* a click, loud enough to clear the idle gate */
        polivoks_fx_unit_process(&f, in, l, r);
        for (int i = 0; i < POLIVOKS_FRAMES; ++i, ++pos) if (fabs((double)l[i]) > bestv) { bestv = fabs((double)l[i]); best = pos; }
    }
    if (bufp) *bufp = buf; else free(buf);
    return best * 1000.0 / sr;
}

int main(void)
{
    for (unsigned i = 0; i < sizeof RATES / sizeof *RATES; ++i) {
        double hz = pitch_hz(RATES[i], 57);                                   /* A3, 220 Hz */
        CHECK(fabs(hz / 220.0 - 1.0) < 0.012, "PLVKS A3 at %.0f Hz sample rate plays %.1f Hz", RATES[i], hz);
    }
    double r0 = release_ms(44100.0f);
    for (unsigned i = 1; i < sizeof RATES / sizeof *RATES; ++i) {
        double r = release_ms(RATES[i]);
        CHECK(fabs(r / r0 - 1.0) < 0.06, "PLVKS release at %.0f Hz lasts %.0f ms (44.1 kHz: %.0f ms)", RATES[i], r, r0);
    }
    for (unsigned t = 0; t < 2; ++t) {
        unsigned pot = t ? 100 : 48;
        double e0 = echo_ms(44100.0f, pot, 0);
        for (unsigned i = 1; i < sizeof RATES / sizeof *RATES; ++i) {
            double e = echo_ms(RATES[i], pot, 0);
            CHECK(fabs(e - e0) < 0.04 * e0 + 2.0, "echo Time %u at %.0f Hz: first repeat at %.1f ms (44.1 kHz: %.1f ms)", pot, RATES[i], e, e0);
        }
    }
    CHECK(polivoks_fx_buffer_len(44100.0f) == 32768u && polivoks_fx_buffer_len(22050.0f) == 32768u, "the delay line is the FM-1's 32768 at 44.1 kHz");
    CHECK(polivoks_fx_buffer_len(96000.0f) >= 65536u, "the delay line grows with the rate (%u at 96 kHz)", polivoks_fx_buffer_len(96000.0f));

    /* two echoes: a click into one must leave the other silent */
    {
        polivoks_fx_t a, b;
        unsigned n = polivoks_fx_buffer_len(48000.0f);
        int16_t *ba = (int16_t *)calloc(n, 2), *bb = (int16_t *)calloc(n, 2);
        int32_t in[POLIVOKS_FRAMES], zero[POLIVOKS_FRAMES] = {0}, l[POLIVOKS_FRAMES], r[POLIVOKS_FRAMES];
        double leak = 0, heard = 0;
        polivoks_fx_unit_init(&a, ba, n, 48000.0f, 0); polivoks_fx_unit_init(&b, bb, n, 48000.0f, 1);
        polivoks_fx_unit_set(&a, FX_DLY_MIX, 127); polivoks_fx_unit_set(&b, FX_DLY_MIX, 127);
        for (int k = 0; k < 600; ++k) {
            for (int i = 0; i < POLIVOKS_FRAMES; ++i) { in[i] = k == 0 ? 8000000 : 0; l[i] = r[i] = 0; }
            polivoks_fx_unit_process(&a, in, l, r);
            for (int i = 0; i < POLIVOKS_FRAMES; ++i) heard += fabs((double)l[i]);
            for (int i = 0; i < POLIVOKS_FRAMES; ++i) l[i] = r[i] = 0;
            polivoks_fx_unit_process(&b, zero, l, r);
            for (int i = 0; i < POLIVOKS_FRAMES; ++i) leak += fabs((double)l[i]) + fabs((double)r[i]);
        }
        CHECK(heard > 1e6 && leak == 0.0, "two echo instances share nothing (heard %.0f, leaked %.0f)", heard, leak);
        free(ba); free(bb);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
