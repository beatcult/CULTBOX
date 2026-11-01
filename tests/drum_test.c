#include <stdio.h>
#include <math.h>
#include "../firmware/src/drums.c"
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static uint32_t st[DRUMS_BANK_BYTES / 4]; static int32_t o[128];
static drums_bank_t *B = (drums_bank_t *)st;
static double run(int blocks, double *pk)
{
    double e = 0; long n = 0; *pk = 0;
    for (int k = 0; k < blocks; k++) {
        for (int i = 0; i < 128; i++) o[i] = 0;
        drums_render(B, o);
        for (int i = 0; i < 128; i++) { double v = o[i] / 8388608.0; e += v * v; n++; if (fabs(v) > *pk) *pk = fabs(v); }
    }
    return sqrt(e / (n ? n : 1));
}
static int blocks_to_idle(void)
{
    double pk;
    for (int k = 0; k < 2000; k++) { run(1, &pk); unsigned any = 0; for (int p = 0; p < DRUMS_NPAD; p++) any |= B->v[p].active; if (!any) return k + 1; }
    return 9999;
}
int main(void)
{
    double pk, r;
    drums_init(B);
    for (int i = 0; i < 128; i++) o[i] = 0;
    drums_render(B, o);
    CHECK(!o[0] && !o[100], "silent until a pad is hit");
    /* every pad sounds, stays below full scale, and ends */
    for (int p = 0; p < DRUMS_NPAD; p++) {
        char m[64];
        drums_init(B);
        CHECK(drums_pad_of_note(drums_note_of_pad(p)) == p, "note map round trip");
        drums_note(B, drums_note_of_pad(p), 100);
        r = run(2, &pk);
        snprintf(m, sizeof m, "pad %d (%s) sounds: rms %.3f peak %.3f", p, drums_names[p], r, pk);
        CHECK(r > 0.02 && pk < 0.95 && pk > 0.2, m);
        int n = blocks_to_idle();
        snprintf(m, sizeof m, "pad %d ends by itself (%d blocks)", p, n);
        CHECK(n < 400, m);
    }
    CHECK(drums_pad_of_note(37) < 0, "an unmapped note is ignored");
    /* tune: up an octave plays in half the time */
    drums_init(B); drums_note(B, 36, 100); int n0 = blocks_to_idle();
    drums_set(B, 0 * 3 + DRUM_TUNE, 127); drums_note(B, 36, 100); int n1 = blocks_to_idle();
    drums_set(B, 0 * 3 + DRUM_TUNE, 0); drums_note(B, 36, 100); int n2 = blocks_to_idle();
    CHECK(n1 < n0 * 0.6 && n2 > n0 * 1.6, "tune up shortens, tune down lengthens (about an octave each way)");
    /* decay */
    drums_init(B); drums_set(B, 9 * 3 + DRUM_DECAY, 20); drums_note(B, 46, 100); int nd = blocks_to_idle();
    drums_set(B, 9 * 3 + DRUM_DECAY, 127); drums_note(B, 46, 100); int nf = blocks_to_idle();
    CHECK(nd < nf / 2, "decay shortens the open hat");
    /* level and velocity */
    drums_init(B); drums_note(B, 38, 127); run(2, &pk); double loud = pk; blocks_to_idle();
    drums_note(B, 38, 40); run(2, &pk); CHECK(pk < loud * 0.5, "velocity scales the hit");
    blocks_to_idle(); drums_set(B, DRUMS_LEVEL, 30); drums_note(B, 38, 127); run(2, &pk); CHECK(pk < loud * 0.4, "master level");
    /* choke */
    drums_init(B); drums_note(B, 46, 100); run(3, &pk); drums_note(B, 42, 100); CHECK(!B->v[HAT_OPEN].active, "closed hat chokes the open hat");
    drums_init(B); drums_set(B, DRUMS_CHOKE, 0); drums_note(B, 46, 100); run(3, &pk); drums_note(B, 42, 100); CHECK(B->v[HAT_OPEN].active, "choke off");
    /* tone and crush stay finite and bounded over random settings */
    unsigned rnd = 4242; int bad = 0;
    for (int t = 0; t < 200; t++) {
        drums_init(B);
        for (int p = 0; p < DRUMS_END; p++) { rnd = rnd * 1664525u + 1013904223u; drums_set(B, p, p == DRUMS_CHOKE ? (rnd >> 24) & 1 : (rnd >> 24) % 128); }
        for (int k = 0; k < 20; k++) { rnd = rnd * 1664525u + 1013904223u; drums_note(B, drums_note_of_pad((rnd >> 24) % DRUMS_NPAD), 1 + (rnd >> 16) % 127); run(3, &pk); if (pk > 1.0 || pk != pk) bad++; }
    }
    CHECK(!bad, "random settings: bounded, no NaN");
    drums_init(B); drums_set(B, DRUMS_TONE, 10); drums_note(B, 42, 100); double dull = run(4, &pk);
    drums_init(B); drums_note(B, 42, 100); double bright = run(4, &pk);
    CHECK(dull < bright * 0.7, "tone turned down darkens (less energy in the hat)");
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
