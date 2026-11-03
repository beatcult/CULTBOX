/* Scales and the arpeggiator: snapping and stepping, the arpeggio of each mode, octaves, rate, gate, latch, recording hook. */
#include <stdio.h>
#include <string.h>
#include "../firmware/src/scale.h"
#include "../firmware/src/arp.c"

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); printf(__VA_ARGS__); printf(" %s\n", ok_ ? "ok" : "FAIL"); fails += !ok_; } while (0)

static unsigned on_log[256], on_n, off_n, rec_n, sounding;
static float on_at[256];
static float clock_now;
static void out(void *c, unsigned n, unsigned v) { (void)c; if (v) { if (on_n < 256u) { on_log[on_n] = n; on_at[on_n] = clock_now; } on_n++; sounding = n; } else { off_n++; if (sounding == n) sounding = 255; } }
static void rec(void *c, unsigned n, unsigned v) { (void)c; (void)n; (void)v; rec_n++; }
static void reset(arp_t *a) { arp_init(a, out, rec, 0); on_n = off_n = rec_n = 0; sounding = 255; clock_now = 0; }
static void run(arp_t *a, float beat, int blocks) { for (int i = 0; i < blocks; i++) { arp_block(a, beat); clock_now += 128; } }
static int seq_is(const unsigned *want, unsigned n) { if (on_n < n) return 0; for (unsigned i = 0; i < n; i++) if (on_log[i] != want[i]) return 0; return 1; }

int main(void)
{
    /* scales */
    CHECK(scale_snap(0, 1, 61) == 60 && scale_snap(0, 1, 63) == 62 && scale_snap(0, 1, 66) == 65, "C major: C# -> C, D# -> D, F# -> F");
    CHECK(scale_snap(0, 0, 61) == 61, "chromatic leaves notes alone");
    CHECK(scale_snap(9, 2, 61) == 60 || scale_snap(9, 2, 61) == 59 || scale_snap(9, 2, 61) == 62, "A minor snaps near");
    CHECK(scale_has(9, 2, 69) && scale_has(9, 2, 71) && scale_has(9, 2, 72) && !scale_has(9, 2, 70), "A minor has A B C, not Bb");
    CHECK(scale_step(0, 1, 60, 1) == 62 && scale_step(0, 1, 62, -1) == 60 && scale_step(0, 1, 61, 1) == 62 && scale_step(0, 1, 61, -1) == 60, "C major: stepping by degrees, from out of scale too");
    CHECK(scale_step(0, 0, 60, 1) == 61 && scale_step(0, 0, 0, -1) == 0 && scale_step(0, 0, 127, 1) == 127, "chromatic steps by semitones, held at the ends");
    CHECK(scale_step(0, 8, 60, 1) == 62 && scale_step(0, 8, 64, 1) == 67, "C pentatonic: C D E G A");
    { int all = 1; for (unsigned s = 0; s < SCALE_N; s++) for (unsigned r = 0; r < 12; r++) for (unsigned n = 0; n < 128; n++) { unsigned q = scale_snap(r, s, n); if (!scale_has(r, s, (int)q) && s) all = 0; } CHECK(all, "every snapped note is in its scale (all roots, all scales)"); }

    /* arpeggio */
    arp_t a;
    float beat = 22050.0f;                       /* 120 BPM */
    reset(&a); arp_set(&a, ARP_ON, 1);
    arp_note(&a, 67, 100); arp_note(&a, 60, 100); arp_note(&a, 64, 100);
    run(&a, beat, 400);
    { const unsigned up[] = {60, 64, 67, 60, 64, 67}; CHECK(seq_is(up, 6), "up: 60 64 67 repeating"); }
    float dt = on_at[1] - on_at[0];
    CHECK(dt > 5512.5f - 130 && dt < 5512.5f + 130, "1/16 at 120 BPM: a note every %.0f samples", dt);
    CHECK(rec_n == on_n, "every note reaches the recording hook (%u)", rec_n);
    CHECK(off_n + 1 >= on_n, "every note but the sounding one has been let go");
    arp_set(&a, ARP_MODE, ARP_DOWN); on_n = 0; arp_stop(&a); arp_note(&a, 60, 100); arp_note(&a, 64, 100); arp_note(&a, 67, 100); run(&a, beat, 400);
    { const unsigned dn[] = {67, 64, 60, 67, 64, 60}; CHECK(seq_is(dn, 6), "down: 67 64 60 repeating"); }
    arp_set(&a, ARP_MODE, ARP_UPDOWN); on_n = 0; arp_stop(&a); arp_note(&a, 60, 100); arp_note(&a, 64, 100); arp_note(&a, 67, 100); run(&a, beat, 500);
    { const unsigned ud[] = {60, 64, 67, 64, 60, 64, 67, 64}; CHECK(seq_is(ud, 8), "up-down: 60 64 67 64 60 64 67 64 (no repeated ends)"); }
    arp_set(&a, ARP_MODE, ARP_ORDER); on_n = 0; arp_stop(&a); arp_note(&a, 67, 100); arp_note(&a, 60, 100); arp_note(&a, 64, 100); run(&a, beat, 400);
    { const unsigned od[] = {67, 60, 64, 67, 60, 64}; CHECK(seq_is(od, 6), "order: as played"); }
    arp_set(&a, ARP_MODE, ARP_UP); arp_set(&a, ARP_OCTS, 1); on_n = 0; arp_stop(&a); arp_note(&a, 60, 100); arp_note(&a, 64, 100); run(&a, beat, 400);
    { const unsigned oc[] = {60, 64, 72, 76, 60, 64}; CHECK(seq_is(oc, 6), "two octaves: 60 64 72 76"); }
    arp_set(&a, ARP_MODE, ARP_RANDOM); arp_set(&a, ARP_OCTS, 0); on_n = 0; arp_stop(&a); arp_note(&a, 60, 100); arp_note(&a, 64, 100); arp_note(&a, 67, 100); run(&a, beat, 2000);
    { int ok = on_n > 30, seen[3] = {0, 0, 0}; for (unsigned i = 0; i < 30 && i < on_n; i++) { if (on_log[i] == 60) seen[0] = 1; else if (on_log[i] == 64) seen[1] = 1; else if (on_log[i] == 67) seen[2] = 1; else ok = 0; } CHECK(ok && seen[0] && seen[1] && seen[2], "random: only the chord, all of it"); }

    /* rate and gate */
    reset(&a); arp_set(&a, ARP_ON, 1); arp_set(&a, ARP_RATE, 3); arp_note(&a, 60, 100); run(&a, beat, 300);
    dt = on_at[1] - on_at[0]; CHECK(dt > 2756.25f - 130 && dt < 2756.25f + 130, "1/32: a note every %.0f samples", dt);
    reset(&a); arp_set(&a, ARP_ON, 1); arp_set(&a, ARP_RATE, 2); arp_note(&a, 60, 100); run(&a, beat, 300);
    dt = on_at[1] - on_at[0]; CHECK(dt > 3675 - 130 && dt < 3675 + 130, "1/16 triplet: a note every %.0f samples", dt);
    reset(&a); arp_set(&a, ARP_ON, 1); arp_set(&a, ARP_GATE, 20); arp_note(&a, 60, 100);
    run(&a, beat, 12); CHECK(sounding == 255, "a short gate: the note has let go before the next");
    reset(&a); arp_set(&a, ARP_ON, 1); arp_set(&a, ARP_GATE, 127); arp_note(&a, 60, 100);
    run(&a, beat, 30); CHECK(sounding == 60, "a long gate: still sounding");

    /* releasing */
    reset(&a); arp_set(&a, ARP_ON, 1); arp_note(&a, 60, 100); arp_note(&a, 64, 100); run(&a, beat, 100);
    arp_note(&a, 60, 0); arp_note(&a, 64, 0); run(&a, beat, 10); unsigned n0 = on_n; run(&a, beat, 200);
    CHECK(on_n == n0 && sounding == 255, "keys let go: silence");

    /* latch */
    reset(&a); arp_set(&a, ARP_ON, 2); arp_note(&a, 60, 100); arp_note(&a, 64, 100); run(&a, beat, 100);
    arp_note(&a, 60, 0); arp_note(&a, 64, 0); n0 = on_n; run(&a, beat, 200);
    CHECK(on_n > n0 + 3, "latch: it plays on after the keys are let go");
    arp_note(&a, 72, 100); on_n = 0; run(&a, beat, 100);
    { int ok = on_n > 2; for (unsigned i = 0; i < on_n && i < 256; i++) if (on_log[i] != 72) ok = 0; CHECK(ok, "latch: a new key starts a new chord"); }
    arp_set(&a, ARP_ON, 0); CHECK(sounding == 255 || sounding == 72, "latch off: the arpeggio stops");

    /* off passes keys on */
    reset(&a); CHECK(arp_note(&a, 60, 100) == 0, "arp off: a key is not taken");
    reset(&a); arp_set(&a, ARP_ON, 1); arp_note(&a, 60, 100); run(&a, beat, 10); arp_set(&a, ARP_ON, 0);
    CHECK(sounding == 60, "arp turned off with a key down: the key sounds");

    printf("%d failure(s)\n", fails);
    return fails != 0;
}
