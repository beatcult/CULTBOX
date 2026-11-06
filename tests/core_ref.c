/* SPDX-License-Identifier: GPL-3.0-only */
/* Regression fingerprint of the PLVKS voice and the BBD echo at the firmware's own 44.1 kHz, through the original
 * (global) API only. It prints one hash per scenario; the plugin core must print the same lines as the firmware did
 * (tests/core_check.sh compares a build of main with the working tree).
 *   cc -O1 -o core_ref tests/core_ref.c && ./core_ref */
#include <stdio.h>
#include <stdint.h>
#include "../firmware/src/polivoks.c"
#include "../firmware/src/polivoks_fx.c"

static uint32_t storage[POLIVOKS_BANK_BYTES / 4];
static polivoks_bank_t *B = (polivoks_bank_t *)storage;
static uint64_t h;
static void mix(int32_t v)
{
    uint32_t u = (uint32_t)v;
    for (int i = 0; i < 4; ++i) { h ^= (u >> (8 * i)) & 255u; h *= 1099511628211ull; }
}
static void blocks(int n)
{
    int32_t out[POLIVOKS_FRAMES];
    for (int k = 0; k < n; ++k) {
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) out[i] = 0;
        polivoks_bank_render(B, out);
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) mix(out[i]);
    }
}
static void report(const char *name) { printf("%-28s %016llx\n", name, (unsigned long long)h); h = 1469598103934665603ull; }

static void voice(void)
{
    h = 1469598103934665603ull;
    polivoks_bank_init(B); polivoks_bank_note(B, 48, 100, 0); blocks(300); polivoks_bank_note(B, 48, 0, 0); blocks(300);
    report("default mono note");

    polivoks_bank_init(B); polivoks_bank_set(B, POLIVOKS_VOICES, POLIVOKS_DUO);
    polivoks_bank_note(B, 43, 100, 0); polivoks_bank_note(B, 62, 100, 0); blocks(300);
    polivoks_bank_note(B, 43, 0, 0); blocks(200); polivoks_bank_note(B, 62, 0, 0); blocks(200);
    report("duophonic pair");

    polivoks_bank_init(B); polivoks_bank_set(B, POLIVOKS_VOICES, POLIVOKS_UNISON); polivoks_bank_set(B, POLIVOKS_DETUNE, 90);
    polivoks_bank_set(B, POLIVOKS_MODE, POLIVOKS_MODE_BP); polivoks_bank_set(B, POLIVOKS_RESO, 110);
    polivoks_bank_note(B, 55, 100, 0); blocks(400);
    report("unison band-pass, high reso");

    polivoks_bank_init(B); polivoks_bank_set(B, POLIVOKS_LOOP, POLIVOKS_LOOP_BOTH); polivoks_bank_set(B, POLIVOKS_GATE, 1);
    polivoks_bank_set(B, POLIVOKS_LFOSHAPE, POLIVOKS_LFO_RANDOM); polivoks_bank_set(B, POLIVOKS_LFO_FILTER, 80);
    polivoks_bank_set(B, POLIVOKS_LFO_OSC1, 40); polivoks_bank_set(B, POLIVOKS_FM, 50); polivoks_bank_set(B, POLIVOKS_NOISE, 30);
    polivoks_bank_note(B, 36, 100, 0); blocks(600); polivoks_bank_note_legato(B, 41, 100); blocks(300);
    report("looping envs, S&H LFO, FM, slide");

    polivoks_bank_init(B); polivoks_bank_vintage(B, 100); polivoks_bank_set(B, POLIVOKS_GLIDE, 70);
    polivoks_bank_set(B, POLIVOKS_LFOSHAPE, POLIVOKS_LFO_NOISE); polivoks_bank_set(B, POLIVOKS_LFO_AMP, 60);
    polivoks_bank_bend(B, 2 << 16); polivoks_bank_mod(B, 90);
    polivoks_bank_note(B, 52, 100, 15); blocks(300); polivoks_bank_note(B, 60, 100, -20); blocks(1200);
    polivoks_bank_note(B, 60, 0, 0); polivoks_bank_note(B, 52, 0, 0); blocks(600);
    report("vintage, glide, noise LFO, bend");

    polivoks_bank_init(B); polivoks_bank_set(B, POLIVOKS_CUTOFF, 127); polivoks_bank_set(B, POLIVOKS_DRIVE, 127);
    polivoks_bank_set(B, POLIVOKS_WAVE1, POLIVOKS_WAVE_PULSE12); polivoks_bank_set(B, POLIVOKS_WAVE2, POLIVOKS_WAVE_PULSE30);
    polivoks_bank_set(B, POLIVOKS_RANGE1, 4); polivoks_bank_set(B, POLIVOKS_RANGE2, 0);
    polivoks_bank_note(B, 100, 100, 0); blocks(300);
    report("extremes: pulses, top note");
}

static void echo(void)
{
    int32_t in[POLIVOKS_FRAMES], l[POLIVOKS_FRAMES], r[POLIVOKS_FRAMES];
    static const uint8_t cfg[3][4] = {{48, 62, 60, 60}, {100, 120, 20, 127}, {10, 40, 127, 90}};
    h = 1469598103934665603ull;
    for (int c = 0; c < 3; ++c) {
        polivoks_fx_reset();
        for (int p = 0; p < 4; ++p) polivoks_fx_set(0, p, cfg[c][p]);
        polivoks_bank_init(B); polivoks_bank_note(B, 48, 100, 0);
        for (int k = 0; k < 1500; ++k) {
            if (k == 200) polivoks_bank_note(B, 48, 0, 0);
            if (k == 700) polivoks_fx_set(0, FX_DLY_TIME, 20);
            for (int i = 0; i < POLIVOKS_FRAMES; ++i) in[i] = l[i] = r[i] = 0;
            polivoks_bank_render(B, in);
            polivoks_fx_process(0, in, l, r);
            for (int i = 0; i < POLIVOKS_FRAMES; ++i) { mix(l[i]); mix(r[i]); }
        }
        report(c == 0 ? "echo default" : c == 1 ? "echo long, feedback, dark" : "echo short, bright, wet");
    }
    /* tempo-synced, both units */
    polivoks_fx_reset(); polivoks_fx_set(1, FX_DLY_MIX, 100); polivoks_fx_set(1, FX_DLY_FEED, 90); polivoks_fx_sync(1, 1, 5);
    polivoks_bank_init(B); polivoks_bank_note(B, 60, 100, 0);
    for (int k = 0; k < 1200; ++k) {
        if (k == 150) polivoks_bank_note(B, 60, 0, 0);
        if (k % 100 == 0) polivoks_fx_tempo(44100.0f * 60.0f / (90.0f + (float)(k / 100) * 6.0f));
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) in[i] = l[i] = r[i] = 0;
        polivoks_bank_render(B, in);
        polivoks_fx_process(1, in, l, r);
        for (int i = 0; i < POLIVOKS_FRAMES; ++i) { mix(l[i]); mix(r[i]); }
    }
    report("echo tempo-synced, unit 1");
}

int main(void)
{
    voice();
    echo();
    return 0;
}
