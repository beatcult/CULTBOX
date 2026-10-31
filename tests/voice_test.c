/* Polivoks voice: structure checks and a stability sweep. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../firmware/src/polivoks.c"
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static uint32_t storage[POLIVOKS_BANK_BYTES / 4];
static polivoks_bank_t *B = (polivoks_bank_t *)storage;
static int32_t buf[128];
static double render(int blocks, double *peak, double *zc_rate)
{
    double e = 0; long zc = 0, n = 0; double prev = 0; *peak = 0;
    for (int k = 0; k < blocks; ++k) {
        for (int i = 0; i < 128; ++i) buf[i] = 0;
        polivoks_bank_render(B, buf);
        for (int i = 0; i < 128; ++i) {
            double v = buf[i] / 8388608.0; e += v * v; ++n;
            if (fabs(v) > *peak) *peak = fabs(v);
            { if ((v >= 0) != (prev >= 0)) ++zc; prev = v; }
        }
    }
    *zc_rate = zc * 44100.0 / (n * 2.0);
    return sqrt(e / (n ? n : 1));
}
static void fresh(void) { polivoks_bank_init(B); }
int main(void)
{
    double pk, zc, rms;
    /* duo: two notes through one path */
    fresh(); polivoks_bank_set(B, POLIVOKS_CUTOFF, 127); polivoks_bank_set(B, POLIVOKS_RESO, 0);
    polivoks_bank_set(B, POLIVOKS_ENVAMT, 0); polivoks_bank_set(B, POLIVOKS_WAVE1, 0); polivoks_bank_set(B, POLIVOKS_WAVE2, 0);
    polivoks_bank_set(B, POLIVOKS_DETUNE, 64); polivoks_bank_set(B, POLIVOKS_DRIVE, 0);
    polivoks_bank_note(B, 48, 100, 0); render(100, &pk, &zc);
    rms = render(200, &pk, &zc);
    CHECK(rms > 0.02 && pk < 0.95, "one note sounds, not clipped");
    double zc1 = zc;
    polivoks_bank_note(B, 60, 100, 0); render(100, &pk, &zc);
    rms = render(200, &pk, &zc);
    CHECK(zc > zc1 * 1.4, "duo: the upper note adds a higher pitch");
    polivoks_bank_set(B, POLIVOKS_VOICES, POLIVOKS_MONO); render(100, &pk, &zc);
    render(200, &pk, &zc);
    CHECK(zc > zc1 * 1.6 && zc < zc1 * 2.4, "mono: both oscillators play the last note (one pitch, an octave up)");
    /* release to idle */
    polivoks_bank_note(B, 60, 0, 0); polivoks_bank_note(B, 48, 0, 0);
    int idle = 0; for (int k = 0; k < 2000 && !idle; ++k) { render(1, &pk, &zc); idle = !polivoks_bank_active(B); }
    CHECK(idle, "released notes fade to idle");
    /* gate drone */
    fresh(); polivoks_bank_set(B, POLIVOKS_GATE, 1); polivoks_bank_note(B, 55, 100, 0); polivoks_bank_note(B, 55, 0, 0);
    rms = render(400, &pk, &zc); CHECK(rms > 0.01, "GATE holds the last note after release");
    polivoks_bank_set(B, POLIVOKS_GATE, 0); idle = 0; for (int k = 0; k < 2000 && !idle; ++k) { render(1, &pk, &zc); idle = !polivoks_bank_active(B); }
    CHECK(idle, "GATE off lets it go");
    /* FM changes the spectrum */
    fresh(); polivoks_bank_set(B, POLIVOKS_CUTOFF, 127); polivoks_bank_set(B, POLIVOKS_RESO, 0); polivoks_bank_set(B, POLIVOKS_ENVAMT, 0);
    polivoks_bank_set(B, POLIVOKS_WAVE1, 0); polivoks_bank_set(B, POLIVOKS_WAVE2, 0); polivoks_bank_set(B, POLIVOKS_LEVEL2, 0);
    polivoks_bank_set(B, POLIVOKS_RANGE2, 0); polivoks_bank_set(B, POLIVOKS_DRIVE, 0);
    polivoks_bank_note(B, 60, 100, 0); render(50, &pk, &zc); render(200, &pk, &zc); double clean = zc;
    polivoks_bank_set(B, POLIVOKS_FM, 90); render(50, &pk, &zc); render(200, &pk, &zc);
    CHECK(fabs(zc - clean) > clean * 0.05, "FM from oscillator II alters oscillator I");
    /* loop restarts */
    fresh(); polivoks_bank_set(B, POLIVOKS_FDECAY, 20); polivoks_bank_set(B, POLIVOKS_FSUSTAIN, 0); polivoks_bank_set(B, POLIVOKS_LOOP, POLIVOKS_LOOP_FILTER);
    polivoks_bank_note(B, 50, 100, 0); int restarts = 0; uint8_t last = B->fstage;
    for (int k = 0; k < 600; ++k) { render(1, &pk, &zc); if (B->fstage == 1 && last != 1) ++restarts; last = B->fstage; }
    CHECK(restarts > 3, "multiple mode makes the filter envelope repeat");
    /* stability: random settings, notes, extremes */
    uint32_t r = 12345; int bad = 0;
    for (int trial = 0; trial < 400; ++trial) {
        fresh();
        for (unsigned p = 0; p < POLIVOKS_NPARAMS; ++p) {
            r = r * 1664525u + 1013904223u;
            unsigned v = (r >> 16) % (parameters[p].max + 1u);
            if (trial & 1) v = (r >> 20) & 1 ? parameters[p].max : 0;
            polivoks_bank_set(B, p, v);
        }
        for (int n = 0; n < 4; ++n) { r = r * 1664525u + 1013904223u; polivoks_bank_note(B, 24 + (r >> 16) % 80, 100, 0); }
        for (int k = 0; k < 120; ++k) {
            for (int i = 0; i < 128; ++i) buf[i] = 0;
            polivoks_bank_render(B, buf);
            for (int i = 0; i < 128; ++i) if (buf[i] > 8388607 || buf[i] < -8388608) bad = 1;
            if (B->s1 != B->s1 || B->s2 != B->s2 || B->env != B->env) bad = 1;
        }
    }
    CHECK(!bad, "400 random and extreme settings stay finite and in range");
    printf(fails ? "FAILED\n" : "all passed\n"); return fails != 0;
}
