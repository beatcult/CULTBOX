#include <stdio.h>
#include <math.h>
#include "../firmware/src/dubsiren.c"
static int fails, bad;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static uint32_t st[DUBSIREN_BANK_BYTES / 4]; static int32_t o[128];
static dubsiren_bank_t *B = (dubsiren_bank_t *)st;
static double run(int blocks, double *pk, double *zc)
{
    double e = 0, prev = 0; long n = 0, z = 0; *pk = 0;
    for (int k = 0; k < blocks; k++) {
        for (int i = 0; i < 128; i++) o[i] = 0;
        dubsiren_render(B, o);
        for (int i = 0; i < 128; i++) { double v = o[i] / 8388608.0; e += v * v; n++; if (fabs(v) > *pk) *pk = fabs(v); if ((v >= 0) != (prev >= 0)) z++; prev = v; if (v != v) bad++; }
    }
    *zc = z * 44100.0 / (2.0 * n); return sqrt(e / (n ? n : 1));
}
int main(void)
{
    double pk, zc, r;
    dubsiren_init(B); dubsiren_set(B, SIREN_DEPTH, 0); dubsiren_set(B, SIREN_CUTOFF, 127); dubsiren_set(B, SIREN_RESO, 0);
    CHECK(!dubsiren_render(B, o), "silent until played");
    dubsiren_note(B, 60, 100); run(60, &pk, &zc); r = run(100, &pk, &zc);
    CHECK(r > 0.03 && pk < 0.95, "a key sounds, not clipped");
    double z60 = zc;
    dubsiren_note(B, 72, 100); run(150, &pk, &zc);
    CHECK(zc > z60 * 1.7, "an octave up: about twice the pitch");
    dubsiren_set(B, SIREN_GLIDE, 100); dubsiren_note(B, 48, 100); run(2, &pk, &zc);
    double z_early = zc; run(1800, &pk, &zc);
    CHECK(z_early > zc * 1.2 && zc < z60 * 0.7, "glide slides down to the new key slowly, then arrives");
    dubsiren_note(B, 48, 0); dubsiren_note(B, 72, 0); dubsiren_note(B, 60, 0);
    int idle = 0; for (int k = 0; k < 4000 && !idle; k++) { run(1, &pk, &zc); idle = !B->active; }
    CHECK(idle, "released: fades to idle");
    dubsiren_init(B); dubsiren_set(B, SIREN_HOLD, 1); r = run(300, &pk, &zc);
    CHECK(r > 0.03, "Hold sounds with no key");
    dubsiren_set(B, SIREN_HOLD, 0); idle = 0; for (int k = 0; k < 4000 && !idle; k++) { run(1, &pk, &zc); idle = !B->active; }
    CHECK(idle, "Hold off lets it go");
    /* each LFO shape moves the pitch */
    for (int s = 0; s < 8; s++) {
        dubsiren_init(B); dubsiren_set(B, SIREN_SHAPE, s); dubsiren_set(B, SIREN_DEPTH, 100); dubsiren_set(B, SIREN_RATE, 70);
        dubsiren_set(B, SIREN_CUTOFF, 127); dubsiren_set(B, SIREN_RESO, 0); dubsiren_note(B, 60, 100);
        double lo = 1e9, hi = 0;
        for (int w = 0; w < 100; w++) { run(8, &pk, &zc); if (zc < lo) lo = zc; if (zc > hi) hi = zc; }
        char m[48]; snprintf(m, sizeof m, "shape %d sweeps the pitch (%.0f..%.0f Hz)", s, lo, hi);
        CHECK(hi > lo * 1.3, m);
    }
    /* stability: extreme settings */
    unsigned rnd = 777;
    for (int t = 0; t < 300; t++) {
        dubsiren_init(B);
        for (unsigned p = 0; p < SIREN_NPARAMS; p++) { rnd = rnd * 1664525u + 1013904223u; unsigned v = (rnd >> 16) % (ds_params[p].max + 1u); if (t & 1) v = (rnd >> 20) & 1 ? ds_params[p].max : 0; dubsiren_set(B, p, v); }
        dubsiren_note(B, 20 + (rnd >> 8) % 90, 100);
        for (int k = 0; k < 150; k++) { for (int i = 0; i < 128; i++) o[i] = 0; dubsiren_render(B, o); for (int i = 0; i < 128; i++) if (o[i] > 8388607 || o[i] < -8388608) bad++; }
    }
    CHECK(bad == 0, "300 random and extreme settings stay finite and in range");
    printf(fails ? "FAILED\n" : "all passed\n"); return fails != 0;
}
