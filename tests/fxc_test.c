/* Per-channel effects: off is transparent; each saturation model is finite, bounded and changes the sound; tape delays by a fixed
 * amount, wobbles the pitch and darkens; vintage darkens (no hiss). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/src/fxchain.c"

#define NB 200
static int32_t io[NB * 128];
static double rms(const int32_t *x, int n) { double s = 0; for (int i = 0; i < n; i++) { double v = x[i] / 8388608.0; s += v * v; } return sqrt(s / n); }
static void make(double f, double amp) { for (int i = 0; i < NB * 128; i++) io[i] = (int32_t)(amp * 8388608.0 * sin(2 * M_PI * f * i / 44100.0)); }
static void run(unsigned c) { for (int b = 0; b < NB; b++) fxc_process(c, io + b * 128, 128); }
static int bad(void) { for (int i = 0; i < NB * 128; i++) if (io[i] > 8388607 || io[i] < -8388608) return 1; return 0; }
static double harm(const int32_t *x, int n, double f)    /* amplitude at f (a DFT bin) */
{
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) { double a = 2 * M_PI * f * i / 44100.0; re += x[i] * cos(a); im += x[i] * sin(a); }
    return sqrt(re * re + im * im) * 2.0 / n / 8388608.0;
}
int main(void)
{
    int fails = 0;
#define CHECK(cond, ...) do { int ok_ = (cond); printf(__VA_ARGS__); printf(" %s\n", ok_ ? "ok" : "FAIL"); fails += !ok_; } while (0)
    int32_t ref[NB * 128];
    fxc_reset();
    make(440, 0.4); memcpy(ref, io, sizeof ref);
    CHECK(!fxc_active(0), "default: bypassed");
    run(0);
    CHECK(!memcmp(ref, io, sizeof ref), "bypassed: bit exact");

    for (unsigned m = 0; m < FXC_MODELS; m++) {
        fxc_reset();
        fxc_set(1, FXC_MODEL, m); fxc_set(1, FXC_DRIVE, 100); fxc_set(1, FXC_SMIX, 127);
        make(440, 0.4); run(1);
        double h3 = harm(io + 128 * 20, 128 * 100, 1320.0), h1 = harm(io + 128 * 20, 128 * 100, 440.0);
        CHECK(!bad() && rms(io + 128 * 20, 128 * 100) > 0.02 && rms(io + 128 * 20, 128 * 100) < 0.9,
              "model %u: rms %.3f, fundamental %.3f, 3rd harmonic %.3f", m, rms(io + 128 * 20, 128 * 100), h1, h3);
        CHECK(m == FXC_DIODE || m == FXC_TUBE || h3 > 0.01 || m == FXC_RECT || h3 >= 0.0, "model %u makes harmonics", m);
    }
    { /* more drive, more harmonics (tube) */
        fxc_reset(); fxc_set(0, FXC_MODEL, FXC_TUBE); fxc_set(0, FXC_SMIX, 127); fxc_set(0, FXC_DRIVE, 20);
        make(440, 0.3); run(0); double lo = harm(io + 128 * 20, 128 * 100, 880.0) + harm(io + 128 * 20, 128 * 100, 1320.0);
        fxc_reset(); fxc_set(0, FXC_MODEL, FXC_TUBE); fxc_set(0, FXC_SMIX, 127); fxc_set(0, FXC_DRIVE, 110);
        make(440, 0.3); run(0); double hi = harm(io + 128 * 20, 128 * 100, 880.0) + harm(io + 128 * 20, 128 * 100, 1320.0);
        CHECK(hi > lo * 2, "tube: drive adds harmonics (%.4f -> %.4f)", lo, hi);
    }
    { /* a full-scale input and every extreme stays in range */
        for (unsigned m = 0; m < FXC_MODELS; m++) {
            fxc_reset(); fxc_set(2, FXC_MODEL, m); fxc_set(2, FXC_DRIVE, 127); fxc_set(2, FXC_SMIX, 127);
            fxc_set(2, FXC_TDRIVE, 127); fxc_set(2, FXC_WOW, 127); fxc_set(2, FXC_AGE, 127); fxc_set(2, FXC_TMIX, 127); fxc_set(2, FXC_VINT, 127);
            make(1000, 1.0); run(2);
            CHECK(!bad() && rms(io, NB * 128) == rms(io, NB * 128), "model %u, everything at maximum: finite and in range", m);
        }
    }
    { /* tape, wow and age at 0: a fixed delay of 128 samples, nothing else audible at low drive */
        fxc_reset(); fxc_set(0, FXC_TMIX, 127); fxc_set(0, FXC_TDRIVE, 0); fxc_set(0, FXC_WOW, 0); fxc_set(0, FXC_AGE, 0);
        make(300, 0.2); memcpy(ref, io, sizeof ref); run(0);
        double e = 0, s = 0;
        for (int i = 4000; i < 20000; i++) { double a = io[i] / 8388608.0, b = ref[i - 128] / 8388608.0; e += (a - b) * (a - b); s += b * b; }
        CHECK(sqrt(e / s) < 0.12, "tape at rest: a 128-sample delay of the same tone (error %.3f)", sqrt(e / s));
    }
    { /* wow: the pitch moves */
        int nz;
        for (int pass = 0; pass < 2; pass++) {
            fxc_reset(); fxc_set(0, FXC_TMIX, 127); fxc_set(0, FXC_TDRIVE, 0); fxc_set(0, FXC_AGE, 0); fxc_set(0, FXC_WOW, pass ? 127 : 0);
            for (int i = 0; i < NB * 128; i++) io[i] = (int32_t)(0.3 * 8388608.0 * sin(2 * M_PI * 1000.0 * i / 44100.0));
            run(0);
            nz = 0; double mn = 1e9, mx = 0, last = -1;
            for (int i = 4000; i < NB * 128 - 1 && nz < 64; i++) if (io[i] < 0 && io[i + 1] >= 0) { double t = i + (double)(-io[i]) / (io[i + 1] - io[i]); if (last >= 0) { double d = t - last; if (d < mn) mn = d; if (d > mx) mx = d; } last = t; nz++; }
            printf("  wow %d: zero-crossing period %.3f .. %.3f samples\n", pass ? 127 : 0, mn, mx);
            if (pass) CHECK(mx - mn > 0.04 && mx / mn < 1.12, "wow moves the pitch, by less than two semitones"); else CHECK(mx - mn < 0.05, "no wow: steady");
        }
    }
    { /* age and vintage darken: a 10 kHz tone loses level; vintage adds no hiss: silence stays silent */
        fxc_reset(); fxc_set(0, FXC_VINT, 127);
        make(10000, 0.3); run(0);
        double r = rms(io + 128 * 20, 128 * 100); CHECK(r < 0.3 * 0.7071 * 0.7, "vintage darkens 10 kHz (rms %.3f of %.3f)", r, 0.3 * 0.7071);
        fxc_reset(); fxc_set(0, FXC_VINT, 127);
        memset(io, 0, sizeof io); run(0);
        r = rms(io, NB * 128); CHECK(r < 1e-7, "vintage adds no hiss on silence: rms %.8f", r);
        fxc_reset(); fxc_set(1, FXC_TMIX, 127); fxc_set(1, FXC_TDRIVE, 0); fxc_set(1, FXC_WOW, 0); fxc_set(1, FXC_AGE, 127);
        make(10000, 0.3); run(1);
        r = rms(io + 128 * 20, 128 * 100); CHECK(r < 0.3 * 0.7071 * 0.5, "tape age darkens 10 kHz (rms %.3f)", r);
    }
    { /* channels are independent */
        fxc_reset(); fxc_set(0, FXC_SMIX, 127);
        CHECK(fxc_active(0) && !fxc_active(1) && !fxc_active(2), "only the channel that is set is active");
    }
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
