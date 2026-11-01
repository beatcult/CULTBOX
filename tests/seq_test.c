#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "../firmware/src/drums.c"
static long now_t; static long onset[64]; static unsigned onvel[64], onpad[64]; static int nons;
static void hook(drums_bank_t *d, unsigned note, unsigned vel, uint32_t delay)
{
    if (nons < 64) { onset[nons] = now_t + (long)delay; onvel[nons] = vel; onpad[nons] = (unsigned)drums_pad_of_note(note); nons++; }
    drums_note_at(d, note, vel, delay);
}
#define drums_note_at hook
#include "../firmware/src/drumseq.c"
#undef drums_note_at
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static uint32_t dst[DRUMS_BANK_BYTES / 4]; static drums_bank_t *D = (drums_bank_t *)dst;
static seq_t Q; static uint8_t steps[SEQ_STEP_BYTES];
static int32_t o[128];
static void run(long blocks, double clk)     /* n blocks; the MIDI clock (if clk > 0) comes every clk samples */
{
    double nextclk = clk; nons = 0; now_t = 0;
    for (long b = 0; b < blocks; b++) {
        while (clk > 0 && nextclk <= (double)now_t) { seq_clock_in(&Q, steps, D); nextclk += clk; }
        seq_block(&Q, steps, D);
        for (int i = 0; i < 128; i++) o[i] = 0;
        drums_render(D, o);
        now_t += 128;
    }
}
static void fresh(void) { drums_init(D); drums_set(D, DRUMS_TONE, 127); seq_init(&Q); memset(steps, 0, sizeof steps); }
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
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
