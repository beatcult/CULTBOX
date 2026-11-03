#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "../firmware/src/drums.c"
#include "../firmware/src/polivoks.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static long now_t; static long onset[128]; static unsigned onvel[128], onpad[128], onnote[128]; static int ontune[128]; static int nons;
static void hook(drums_bank_t *d, unsigned note, unsigned vel, uint32_t delay, int tune)
{
    if (nons < 128) { onset[nons] = now_t + (long)delay; onvel[nons] = vel; onpad[nons] = (unsigned)drums_pad_of_note(note); onnote[nons] = 0; ontune[nons] = tune; nons++; }
    drums_note_ex(d, note, vel, delay, tune);
}
static long plt[128]; static unsigned pln[128], plv[128]; static int npl;
static void pl_hook(polivoks_bank_t *b, unsigned note, unsigned vel, unsigned x)
{
    (void)b; (void)x;
    if (npl < 128) { plt[npl] = now_t; pln[npl] = note; plv[npl] = vel; npl++; }
}
static void pl_leg(polivoks_bank_t *b, unsigned note, unsigned vel)
{
    (void)b;
    if (npl < 128) { plt[npl] = now_t; pln[npl] = note; plv[npl] = vel | 0x80u; npl++; }
}
#define drums_note_ex hook
#define polivoks_bank_note pl_hook
#define polivoks_bank_note_legato pl_leg
#include "../firmware/src/drumseq.c"
#undef drums_note_ex
#undef polivoks_bank_note
#undef polivoks_bank_note_legato
static uint32_t dst[DRUMS_BANK_BYTES / 4]; static drums_bank_t *D = (drums_bank_t *)dst;
static seq_t Q; static uint8_t steps[SEQ_PAT_BYTES];
static int32_t o[128];
static void run(long blocks, double clk)     /* n blocks; the MIDI clock (if clk > 0) comes every clk samples */
{
    double nextclk = clk; nons = 0; now_t = 0; npl = 0;
    for (long b = 0; b < blocks; b++) {
        while (clk > 0 && nextclk <= (double)now_t) { seq_clock_in(&Q, steps); nextclk += clk; }
        seq_block(&Q, steps);
        for (int i = 0; i < 128; i++) o[i] = 0;
        drums_render(D, o);
        now_t += 128;
    }
}
static void fresh(void) { drums_init(D); drums_set(D, DRUMS_TONE, 127); seq_init(&Q, D, 0); memset(steps, 0, sizeof steps); npl = 0; }
int main(void)
{
    /* kick on every fourth step, 120 BPM, 1/16 steps: a hit every 22050 samples */
    fresh(); for (int s = 0; s < 16; s += 4) seq_put(steps, s, 0, 1);
    seq_command(&Q, SEQ_START); run(2 * 22050 / 128 + 200, 0);
    CHECK(nons >= 4, "four kicks in about two seconds of one bar");
    long d1 = onset[1] - onset[0], d2 = onset[2] - onset[1];
    CHECK(labs(d1 - 22050) < 4 && labs(d2 - 22050) < 4, "a quarter note is 22050 samples at 120 BPM (to the sample)");
    /* the pattern loops: 16 steps = one bar = 88200 samples */
    fresh(); seq_put(steps, 0, 0, 1); seq_command(&Q, SEQ_START); run(3 * 88200 / 128 + 50, 0);
    CHECK(nons >= 3 && labs((onset[1] - onset[0]) - 88200) < 4, "a 16 step pattern loops every bar");
    /* length 32: two bars */
    fresh(); seq_set(&Q, SEQ_LEN, 1); seq_put(steps, 0, 0, 1); seq_command(&Q, SEQ_START); run(3 * 2 * 88200 / 128 + 50, 0);
    CHECK(nons >= 3 && labs((onset[1] - onset[0]) - 176400) < 4, "a 32 step pattern loops every two bars");
    /* rate 1/32: a step is half as long */
    fresh(); seq_set(&Q, SEQ_RATE, 2); seq_put(steps, 0, 0, 1); seq_put(steps, 1, 0, 1); seq_command(&Q, SEQ_START); run(30000 / 128, 0);
    CHECK(nons >= 2 && labs((onset[1] - onset[0]) - 2756) < 4, "1/32 steps are 2756 samples at 120 BPM");
    /* tempo */
    fresh(); seq_set(&Q, SEQ_TEMPO, 200); seq_put(steps, 0, 0, 1); seq_put(steps, 1, 0, 1); seq_command(&Q, SEQ_START); run(30000 / 128, 0);
    CHECK(nons >= 2 && labs((onset[1] - onset[0]) - (long)(44100.0 * 60 / 240 / 4)) < 4, "240 BPM: a step is 2756 samples");
    /* swing: the odd step comes half a step late at the maximum */
    fresh(); seq_set(&Q, SEQ_SWING, 127); seq_put(steps, 0, 0, 1); seq_put(steps, 1, 0, 1); seq_command(&Q, SEQ_START); run(30000 / 128, 0);
    CHECK(nons >= 2 && labs((onset[1] - onset[0]) - (5512 + 2756)) < 6, "swing 75%: the second step is half a step late");
    fresh(); seq_set(&Q, SEQ_SWING, 0); seq_put(steps, 0, 0, 1); seq_put(steps, 1, 0, 1); seq_command(&Q, SEQ_START); run(30000 / 128, 0);
    CHECK(nons >= 2 && labs((onset[1] - onset[0]) - 5512) < 6, "no swing: even steps");
    /* accent */
    fresh(); seq_put(steps, 0, 1, 1); seq_put(steps, 4, 1, 2); seq_command(&Q, SEQ_START); run(30000 / 128, 0);
    CHECK(nons >= 2 && onvel[1] > onvel[0], "an accent is louder than a hit");
    /* stop */
    fresh(); seq_put(steps, 0, 0, 1); seq_command(&Q, SEQ_START); run(100, 0); seq_command(&Q, SEQ_STOP);
    CHECK(!Q.run && Q.pos == SEQ_NONE, "stop");
    /* every pad plays from its own bits */
    fresh(); for (int p = 0; p < DRUMS_NPAD; p++) seq_put(steps, 0, p, 1 + (p & 1));
    int ok = 1; for (int p = 0; p < DRUMS_NPAD; p++) ok &= seq_get(steps, 0, p) == (unsigned)(1 + (p & 1)) && seq_get(steps, 1, p) == 0;
    CHECK(ok, "ten pads x 2 bits a step pack and unpack");
    /* MIDI clock: 24 clocks a quarter; 4 steps a quarter at 1/16 */
    fresh(); seq_set(&Q, SEQ_SYNC, 1); for (int s = 0; s < 16; s++) seq_put(steps, s, 0, 1);
    seq_command(&Q, SEQ_START); run(40000 / 128, 0);
    CHECK(nons == 0, "external sync: no clock, no steps");
    fresh(); seq_set(&Q, SEQ_SYNC, 1); for (int s = 0; s < 16; s++) seq_put(steps, s, 0, 1);
    seq_command(&Q, SEQ_START); run(2 * 22050 / 128, 22050.0 / 24);
    CHECK(Q.pos != SEQ_NONE && nons >= 7 && nons <= 9, "external clock at 120 BPM: eight 16ths in two beats");
    CHECK(nons >= 3 && labs((onset[2] - onset[1]) - 5512) < 300, "external steps follow the clock spacing");
    /* a faster clock speeds it up */
    fresh(); seq_set(&Q, SEQ_SYNC, 1); for (int s = 0; s < 16; s++) seq_put(steps, s, 0, 1);
    seq_command(&Q, SEQ_START); run(2 * 22050 / 128, 22050.0 / 24 / 2);
    CHECK(nons >= 14, "external clock at 240 BPM: twice as many steps");
    /* ---- per-hit edits */
    fresh(); seq_put(steps, 0, 0, 1);
    { seq_hit_t h = {24 + 12, 100, 0, 0, 15}; seq_hit_put(steps, 0, 0, &h); }
    seq_command(&Q, SEQ_START); run(2, 0);
    CHECK(nons == 1 && ontune[0] == 12 && onvel[0] == 100, "an edited hit carries its pitch (+6 semitones) and velocity");
    fresh(); seq_put(steps, 0, 0, 1); { seq_hit_t h = {24, 90, 8, 3, 15}; seq_hit_put(steps, 0, 0, &h); }
    seq_command(&Q, SEQ_START); run(3000 / 128, 0);
    CHECK(nons == 4, "ratchet 3: four hits in the step");
    CHECK(nons == 4 && labs((onset[1] - onset[0]) - 5512 / 4) < 4 && labs((onset[3] - onset[2]) - 5512 / 4) < 4, "ratchet hits are evenly spaced in the step");
    CHECK(nons == 4 && onvel[3] < onvel[0], "ratchet hits get softer");
    CHECK(nons == 4 && onset[0] >= 5512 * 8 / 32 - 4 && onset[0] <= 5512 * 8 / 32 + 4, "delay 8 of 32: a quarter of a step late");
    fresh(); { int ok = 1; for (int i = 0; i < SEQ_NEX; i++) { seq_hit_t h = {24, 50, 0, 0, 15}; ok &= seq_hit_put(steps, i % 64, i / 64 % 10, &h) == 0; seq_put(steps, i % 64, i / 64 % 10, 1); } 
      seq_hit_t h = {24, 50, 0, 0, 15}; CHECK(ok && seq_hit_put(steps, 63, 9, &h) == -1, "the table of edited hits holds 40 and then says it is full"); }
    fresh(); { seq_hit_t h = {30, 77, 3, 1, 9}, g; seq_hit_put(steps, 5, 2, &h); seq_hit_put(steps, 5, 2, &h);
      CHECK(seq_hit_get(steps, 5, 2, &g) && g.tune == 30 && g.vel == 77 && g.delay == 3 && g.ratchet == 1, "an edited hit packs and unpacks");
      seq_hit_del(steps, 5, 2); CHECK(!seq_hit_get(steps, 5, 2, &g), "and can be removed"); }
    /* copy a page: steps and edits go to the next page; the target's own edits are replaced */
    fresh(); { seq_put(steps, 3, 4, 2); seq_put(steps, 19, 1, 1); seq_hit_t h = {20, 60, 1, 1, 15}, g; seq_hit_put(steps, 3, 4, &h); seq_hit_put(steps, 19, 1, &h);
      seq_copy_page(steps, SEQ_KIND_DRUMS, 0, 1);
      CHECK(seq_get(steps, 19, 4) == 2 && seq_get(steps, 3, 4) == 2 && seq_get(steps, 19, 1) == 0, "copy bar: the steps are copied over the next page");
      CHECK(seq_hit_get(steps, 19, 4, &g) && g.vel == 60 && g.ratchet == 1 && !seq_hit_get(steps, 19, 1, &g), "copy bar: the edits go with them, the target's old ones go"); }
    /* ---- PLVKS notes */
    fresh(); seq_kind(&Q, SEQ_KIND_PLVKS);
    { seq_note_t r = {1, 48, 1, 55, 7, 0, 100, 0, 15}, g; seq_note_put(steps, 0, &r); seq_note_get(steps, 0, &g);
      CHECK(g.on && g.n1 == 48 && g.has2 && g.n2 == 55 && g.len == 7 && g.amp == 100, "a PLVKS step packs and unpacks");
      seq_note_t r2 = {1, 50, 0, 0, 15, 0, 80, 0, 15}; seq_note_put(steps, 4, &r2); }
    seq_command(&Q, SEQ_START); run(30000 / 128, 0);
    int on1 = -1, off1 = -1, on2 = -1, off2 = -1, on3 = -1, off3 = -1;
    for (int i = 0; i < npl; i++) { if (pln[i] == 48 && plv[i] && on1 < 0) on1 = i; if (pln[i] == 48 && !plv[i] && off1 < 0) off1 = i; if (pln[i] == 55 && plv[i] && on2 < 0) on2 = i; if (pln[i] == 55 && !plv[i] && off2 < 0) off2 = i; if (pln[i] == 50 && plv[i] && on3 < 0) on3 = i; if (pln[i] == 50 && !plv[i] && off3 < 0) off3 = i; }
    CHECK(on1 >= 0 && off1 > on1 && on2 >= 0 && off2 > on2, "a PLVKS step plays its two notes and switches them off");
    CHECK(on1 >= 0 && labs(plt[on1]) < 200 && plv[on1] == 100, "the first step starts at once with its velocity");
    CHECK(on1 >= 0 && off1 >= 0 && labs((plt[off1] - plt[on1]) - (long)(5512.5 * 8 / 16 * 0.97)) < 300, "length 7 of 16: half a step (to the audio block)");
    CHECK(on3 >= 0 && labs(plt[on3] - 4 * 5512) < 300, "step 5 starts four steps later");
    CHECK(on3 >= 0 && off3 >= 0 && labs((plt[off3] - plt[on3]) - (long)(5512.5 * 0.97)) < 300, "length 15 of 16: a full step, just short of the next");
    seq_command(&Q, SEQ_STOP); fresh(); seq_kind(&Q, SEQ_KIND_PLVKS);
    { seq_note_t r = {1, 60, 0, 0, 15, 0, 100, 0, 15}; seq_note_put(steps, 0, &r); }
    seq_command(&Q, SEQ_START); run(10, 0); npl = 0; seq_command(&Q, SEQ_STOP);
    CHECK(npl >= 1 && pln[0] == 60 && plv[0] == 0, "stop switches off the notes that sound");
    /* ---- chance and slide */
    seq_command(&Q, SEQ_STOP); fresh(); seq_kind(&Q, SEQ_KIND_DRUMS);
    { for (int i = 0; i < 64; i++) seq_put(steps, i, 0, 1); seq_hit_t h = {24, 100, 0, 0, 15}; for (int i = 0; i < 64; i++) seq_hit_put(steps, i, 0, &h); }
    { seq_set(&Q, SEQ_LEN, 3); seq_command(&Q, SEQ_START); run(64 * 5512 / 128 + 4, 0); seq_command(&Q, SEQ_STOP);
      CHECK(nons >= 63 && nons <= 65, "chance 15 of 15: every hit plays"); }
    fresh(); seq_kind(&Q, SEQ_KIND_DRUMS); seq_set(&Q, SEQ_LEN, 3);
    { for (int i = 0; i < 64; i++) seq_put(steps, i, 0, 1); seq_hit_t h = {24, 100, 0, 0, 7}; for (int i = 0; i < 64; i++) seq_hit_put(steps, i, 0, &h); }
    { seq_command(&Q, SEQ_START); run(64 * 5512 / 128 + 4, 0); seq_command(&Q, SEQ_STOP);
      CHECK(nons >= 18 && nons <= 46, "chance 7 of 15: about half the hits play"); }
    fresh(); seq_kind(&Q, SEQ_KIND_PLVKS);
    { seq_note_t a = {1, 48, 0, 0, 7, 0, 100, 0, 15}, b = {1, 55, 0, 0, 7, 0, 100, 1, 15}; seq_note_put(steps, 0, &a); seq_note_put(steps, 1, &b); }
    seq_command(&Q, SEQ_START); run(2 * 5512 / 128 + 4, 0);
    { int onb = -1, offa = -1, offa_early = 0;
      for (int i = 0; i < npl; i++) { if (pln[i] == 55 && plv[i] == (100 | 0x80) && onb < 0) onb = i; if (pln[i] == 48 && !plv[i]) { if (offa < 0) offa = i; } }
      for (int i = 0; i < npl; i++) if (pln[i] == 48 && !plv[i] && plt[i] < 5000) offa_early = 1;
      CHECK(onb >= 0 && labs(plt[onb] - 5512) < 300, "a slide: the second note comes in legato on its step");
      CHECK(offa > onb && !offa_early, "a slide: the note before holds on until the new one is down"); }
    fresh(); seq_kind(&Q, SEQ_KIND_PLVKS);
    { seq_note_t b = {1, 55, 0, 0, 7, 0, 100, 1, 15}; seq_note_put(steps, 0, &b); }
    seq_command(&Q, SEQ_START); run(20, 0);
    { int tr = 0; for (int i = 0; i < npl; i++) if (pln[i] == 55 && plv[i] == 100) tr = 1;
      CHECK(tr, "a slide with no note before it is an ordinary trigger"); }
    /* ---- the scale: notes of the pattern snap as they play; the nearest step */
    fresh(); seq_kind(&Q, SEQ_KIND_PLVKS);
    { seq_note_t a = {1, 61, 1, 66, 7, 0, 100, 0, 15}; seq_note_put(steps, 0, &a); }
    seq_snap(&Q, 1, 0, 1);                                                    /* C major */
    seq_command(&Q, SEQ_START); run(10, 0);
    { int n60 = 0, n65 = 0, n61 = 0; for (int i = 0; i < npl; i++) if (plv[i] == 100) { n60 += pln[i] == 60; n65 += pln[i] == 65; n61 += pln[i] == 61 || pln[i] == 66; }
      CHECK(n60 == 1 && n65 == 1 && n61 == 0, "a scale: C# and F# of the pattern play as C and F"); }
    seq_snap(&Q, 0, 0, 1); seq_command(&Q, SEQ_STOP); seq_command(&Q, SEQ_START); run(10, 0);
    { int n61 = 0; for (int i = 0; i < npl; i++) if (plv[i] == 100 && pln[i] == 61) n61 = 1; CHECK(n61, "the scale off: the pattern plays as written"); }
    seq_snap(&Q, 0, 0, 0); seq_command(&Q, SEQ_STOP);
    fresh(); seq_kind(&Q, SEQ_KIND_PLVKS); seq_set(&Q, SEQ_LEN, 0);
    seq_command(&Q, SEQ_START); run(2, 0);
    CHECK(seq_nearest(&Q) == 0, "nearest step: just after step 0 is step 0");
    { seq_t *q = &Q; int ok = 1; for (long b = 0; b < 40; b++) { now_t = 0; seq_block(q, steps); unsigned nr = seq_nearest(q); if (nr != 0 && nr != 1) ok = 0; } CHECK(ok, "nearest step: always the step playing or the next"); }
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
