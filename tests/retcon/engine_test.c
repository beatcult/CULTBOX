/* SPDX-License-Identifier: GPL-3.0-only */
/* RETCON-1 engine tests on a computer (C99): math, filter, envelope, LFSR, delay, sequencer timing, voice
 * stealing, a random stress run, buffer-size independence. Built by CI: see .github/workflows/build.yml.
 *   cc -std=c99 -O2 -Wall -Wextra -DRC_POOL= -o engine_test tests/retcon/engine_test.c -lm
 *   engine_test [out.wav]   (writes a short chord render when a path is given) */
#define RC_POOL
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/src/engine/retcon_synth.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int16_t line[RC_DELAY_SAMPLES];
static rc_synth_t S, S2;

static void test_math(void)
{
    double we = 0, wt = 0, ws = 0;
    float x;
    printf("[math]\n");
    for (x = -10.0f; x <= 10.0f; x += 0.0137f) { double e = fabs((double)rc_exp2(x) / exp2((double)x) - 1.0); if (e > we) we = e; }
    for (x = 0.001f; x <= 1.2f; x += 0.0011f) { double e = fabs((double)rc_tan(x) / tan((double)x) - 1.0); if (e > wt) wt = e; }
    for (x = 0.0f; x < 1.0f; x += 0.00091f) { double e = fabs((double)rc_sin_turns(x) - sin(6.283185307 * (double)x)); if (e > ws) ws = e; }
    printf("  exp2 %.2e  tan %.2e  sin %.2e\n", we, wt, ws);
    CHECK(we < 5e-5, "exp2 accuracy");
    CHECK(wt < 1e-3, "tan accuracy");
    CHECK(ws < 2e-3, "sin accuracy");
    CHECK(fabsf(rc_midi_hz(69.0f) - 440.0f) < 0.05f, "A4");
    CHECK(fabsf(rc_midi_hz(60.0f) - 261.626f) < 0.1f, "C4");
    CHECK(rc_floor(-0.5f) == -1.0f && rc_floor(2.5f) == 2.0f && rc_floor(-3.0f) == -3.0f, "floor");
    CHECK(rc_is_finite(1.0f) && !rc_is_finite(NAN) && !rc_is_finite(INFINITY), "is_finite");
}

static float svf_gain(int mode, float fc, float res, float f)
{
    rc_svf_t v;
    double pk = 0;
    int i;
    memset(&v, 0, sizeof v);
    rc_svf_init(&v);
    v.mode = (uint8_t)mode;
    rc_svf_set(&v, fc, res);
    for (i = 0; i < 44100; i++) {
        float y = rc_svf_process(&v, sinf(6.2831853f * f * (float)i / RC_SAMPLE_RATE));
        if (i > 20000 && fabs((double)y) > pk) pk = fabs((double)y);
    }
    return (float)pk;
}

static void test_svf(void)
{
    float lp = svf_gain(RC_SVF_LP, 2000, 0, 100), st = svf_gain(RC_SVF_LP, 1000, 0, 8000),
          rs = svf_gain(RC_SVF_LP, 1000, 0.9f, 1000), hp = svf_gain(RC_SVF_HP, 4000, 0, 100),
          bp = svf_gain(RC_SVF_BP, 1000, 0.5f, 1000);
    rc_svf_t v;
    float y = 0;
    int i;
    printf("[svf]\n  LP pass %.3f stop %.4f res %.2f HP stop %.4f BP %.3f\n", (double)lp, (double)st, (double)rs, (double)hp, (double)bp);
    CHECK(lp > 0.97f && lp < 1.03f, "LP passband");
    CHECK(st < 0.03f, "LP stopband");
    CHECK(rs > 3.0f, "resonance peak");
    CHECK(hp < 0.01f, "HP stopband");
    CHECK(bp > 0.9f && bp < 1.1f, "BP unity peak");
    memset(&v, 0, sizeof v);
    rc_svf_init(&v);
    rc_svf_set(&v, 1000, 0.5f);
    rc_svf_process(&v, NAN);
    for (i = 0; i < 100; i++) y = rc_svf_process(&v, 0.1f);
    CHECK(rc_is_finite(y), "SVF recovers from NaN");
}

static void test_adsr(void)
{
    int lin;
    printf("[adsr]\n");
    for (lin = 0; lin < 2; lin++) {
        rc_adsr_t e;
        int i, to_peak = -1, rel = 0;
        float mx = 0, sus;
        memset(&e, 0, sizeof e);
        rc_adsr_init(&e);
        e.linear = (uint8_t)lin;
        rc_adsr_set(&e, 10, 100, 0.5f, 200);
        rc_adsr_on(&e);
        for (i = 0; i < 44100; i++) {
            float v = rc_adsr_process(&e);
            if (v > mx) mx = v;
            if (to_peak < 0 && e.stage != RC_ENV_ATTACK) to_peak = i;
        }
        sus = e.level;
        rc_adsr_off(&e);
        while (rc_adsr_active(&e) && rel < 400000) { rc_adsr_process(&e); rel++; }
        printf("  %s: attack %.1f ms, peak %.3f, sustain %.3f, release %.0f ms\n", lin ? "linear" : "exp", to_peak / 44.1, (double)mx, (double)sus, rel / 44.1);
        CHECK(to_peak > 380 && to_peak < 500, "attack time");
        CHECK(mx <= 1.0001f, "overshoot");
        CHECK(fabsf(sus - 0.5f) < 0.01f, "sustain level");
        CHECK(!rc_adsr_active(&e) && rel > 4400 && rel < 16000, "release time");
    }
}

static void test_lfsr(void)
{
    uint32_t seed = 0xACE1BEEFu, s = seed;
    unsigned long long steps = 0;
    rc_hiss_t h;
    double sum = 0, sum2 = 0;
    int i;
    printf("[lfsr]\n");
    do { s = (s >> 1) ^ ((0u - (s & 1u)) & 0xD0000001u); steps++; } while (s != seed && steps < 5000000000ull);
    printf("  period %llu\n", steps);
    CHECK(steps == 4294967295ull, "LFSR maximal length");
    rc_hiss_init(&h);
    rc_hiss_set(&h, 1.0f);
    for (i = 0; i < 200000; i++) { float v = rc_hiss_process(&h); sum += (double)v; sum2 += (double)v * (double)v; }
    printf("  hiss mean %.2e rms %.4f\n", sum / 200000, sqrt(sum2 / 200000));
    CHECK(fabs(sum / 200000) < 1e-3, "hiss DC");
    CHECK(sqrt(sum2 / 200000) > 0.003 && sqrt(sum2 / 200000) < 0.03, "hiss level");
}

static void test_delay(void)
{
    rc_delay_t d;
    int i, first = -1, second = -1;
    float p1 = 0, p2 = 0, mx = 0;
    printf("[delay]\n");
    memset(line, 0, sizeof line);
    memset(&d, 0, sizeof d);
    rc_delay_init(&d, line);
    rc_delay_mix(&d, 1.0f);
    rc_delay_time_ms(&d, 100.0f);
    d.delay_cur = d.delay_target;
    rc_delay_feedback(&d, 0.5f);
    rc_delay_damp_hz(&d, 8000.0f);
    for (i = 0; i < 22000; i++) {
        float y = rc_delay_process(&d, i == 0 ? 0.8f : 0.0f);
        if (i > 4200 && i < 4600 && fabsf(y) > p1) { p1 = fabsf(y); first = i; }
        if (i > 8600 && i < 9200 && fabsf(y) > p2) { p2 = fabsf(y); second = i; }
    }
    printf("  echo 1 at %d (%.3f), echo 2 at %d (%.3f)\n", first, (double)p1, second, (double)p2);
    CHECK(first >= 4405 && first <= 4415, "echo 1 position");
    CHECK(second >= 8815 && second <= 8825, "echo 2 position");
    CHECK(p2 < p1 && p2 > 0.05f, "feedback decay");
    memset(line, 0, sizeof line);
    memset(&d, 0, sizeof d);
    rc_delay_init(&d, line);
    rc_delay_mix(&d, 1.0f);
    d.feedback = 1.0f;
    rc_delay_mod(&d, 60.0f);
    for (i = 0; i < 300000; i++) {
        float y = rc_delay_process(&d, i < 5000 ? 0.9f : 0.0f);
        if (!rc_is_finite(y)) { CHECK(0, "delay non-finite"); break; }
        if (fabsf(y) > mx) mx = fabsf(y);
    }
    printf("  max-feedback peak %.3f\n", (double)mx);
    CHECK(mx <= 1.0f, "delay bounded at max feedback");
}

static void test_seq(void)
{
    rc_seq_t q;
    rc_seq_ev_t ev;
    int i, on_count = 0, first_on = -1, second_on = -1, off_count = 0;
    uint32_t t = 0;
    printf("[sequencer]\n");
    memset(&q, 0, sizeof q);
    rc_seq_init(&q);
    q.on = 0xFFFFu;
    q.bpm = 120;
    q.gate_pct = 50;
    rc_seq_start(&q);
    /* 120 BPM, 1/16 = 125 ms = 5512 samples. Run 2 s of control ticks. */
    for (i = 0; i < 88200 / RC_CTRL_DIV; i++, t += RC_CTRL_DIV) {
        rc_seq_advance(&q, RC_CTRL_DIV, &ev);
        if (ev.on >= 0) { on_count++; if (first_on < 0) first_on = (int)t; else if (second_on < 0) second_on = (int)t; }
        if (ev.off >= 0) off_count++;
    }
    printf("  16th notes in 2 s at 120 BPM: %d (expect 16), spacing %d samples (expect ~5512)\n", on_count, second_on - first_on);
    CHECK(on_count >= 15 && on_count <= 17, "step count");
    CHECK(abs((second_on - first_on) - 5512) <= RC_CTRL_DIV, "step spacing");
    CHECK(off_count >= on_count - 1, "every note gets a gate-off");
    /* swing: even steps long, odd short, pair unchanged */
    q.swing_pct = 25;
    CHECK(rc_seq_len(&q, 0) + rc_seq_len(&q, 1) == 2u * (661500u / 120u), "swing keeps the pair length");
    /* modes visit all steps / stay in range */
    {
        int m, k;
        for (m = 0; m < 4; m++) {
            unsigned seen = 0;
            memset(&q, 0, sizeof q);
            rc_seq_init(&q);
            q.mode = (uint8_t)m;
            rc_seq_start(&q);
            for (k = 0; k < 200; k++) { unsigned s = rc_seq_next(&q); if (s > 15u) { CHECK(0, "step out of range"); break; } q.step = (uint8_t)s; seen |= 1u << s; }
            CHECK(m == RC_SEQ_RAND ? seen > 0x7FFFu / 4 : seen == 0xFFFFu, "mode %d coverage %x", m, seen);
        }
    }
}

static void write_wav(const char *path, const int16_t *m, size_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t sr = 44100, bytes = (uint32_t)(n * 2), riff = 36 + bytes, fmt = 16, br = sr * 2;
    uint16_t pcm = 1, ch = 1, ba = 2, bits = 16;
    if (!f) return;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f); fwrite(m, 2, n, f);
    fclose(f);
}

static void fresh(rc_synth_t *s)
{
    memset(line, 0, sizeof line);
    memset(s, 0, sizeof *s);
    rc_synth_init(s, line);
}

static void test_synth(const char *wav)
{
    static float buf[44100 * 2];
    static int16_t pcm[44100 * 8];
    uint32_t i, rng = 12345;
    int n, blk, peak = 0, ok = 1;
    double sum2 = 0;
    printf("[synth]\n");

    fresh(&S);
    for (n = 48; n < 54; n++) rc_synth_note_on(&S, (unsigned)n, 100);
    CHECK(rc_alloc_active(&S.alloc) == RC_POLYPHONY, "4 voices active");
    {
        int held = 0, oldest = 0;
        for (i = 0; i < RC_POLYPHONY; i++) {
            held += S.alloc.voices[i].gate;
            oldest += (S.alloc.voices[i].note == 48 || S.alloc.voices[i].note == 49);
        }
        CHECK(held == RC_POLYPHONY, "all gated");
        CHECK(oldest == 0, "oldest notes were stolen");
    }
    rc_synth_all_off(&S);
    for (blk = 0; blk < 4; blk++) rc_synth_render(&S, buf, 44100);
    CHECK(rc_alloc_active(&S.alloc) == 0, "voices idle after the release tail");

    fresh(&S);
    rc_synth_note_on(&S, 48, 100); rc_synth_note_on(&S, 55, 100); rc_synth_note_on(&S, 59, 100); rc_synth_note_on(&S, 64, 100);
    for (blk = 0; blk < 8; blk++) {
        if (blk == 3) { rc_synth_note_off(&S, 48); rc_synth_note_off(&S, 55); rc_synth_note_off(&S, 59); rc_synth_note_off(&S, 64); }
        rc_synth_render(&S, buf, 44100);
        for (i = 0; i < 44100; i++) {
            int v = (int)(rc_clamp(buf[i], -1.0f, 1.0f) * 32767.0f);
            pcm[(uint32_t)blk * 44100u + i] = (int16_t)v;
            if (abs(v) > peak) peak = abs(v);
            sum2 += (double)v * (double)v;
            if (!rc_is_finite(buf[i])) ok = 0;
        }
    }
    printf("  chord: peak %d (%.1f dBFS) rms %.0f\n", peak, 20.0 * log10(peak / 32767.0), sqrt(sum2 / (44100.0 * 8.0)));
    CHECK(ok, "chord render finite");
    CHECK(peak > 3000 && peak < 32700, "chord level");
    if (wav) write_wav(wav, pcm, 44100u * 8u);

    /* sequencer through the engine: it must make sound by itself */
    fresh(&S);
    rc_synth_seq_run(&S, 1);
    peak = 0;
    for (blk = 0; blk < 2; blk++) {
        rc_synth_render(&S, buf, 44100);
        for (i = 0; i < 44100; i++) { int v = (int)(fabsf(buf[i]) * 32767.0f); if (v > peak) peak = v; }
    }
    printf("  sequencer-only: peak %d\n", peak);
    CHECK(peak > 2000, "sequencer plays");
    rc_synth_seq_run(&S, 0);
    for (blk = 0; blk < 5; blk++) rc_synth_render(&S, buf, 44100);
    CHECK(rc_alloc_active(&S.alloc) == 0, "stopping the sequencer releases its note");

    /* stress: random notes and parameters, including 0 and 1 extremes */
    fresh(&S);
    ok = 1;
    for (blk = 0; blk < 3000 && ok; blk++) {
        rng = rng * 1664525u + 1013904223u;
        if (blk % 4 == 0) {
            unsigned id = (rng >> 8) % RC_P_COUNT, r = (rng >> 16) & 7u;
            rc_synth_param127(&S, id, r == 0 ? 0u : r == 7 ? 127u : (rng >> 4) & 127u);
        }
        if (blk % 3 == 0) rc_synth_note_on(&S, 24u + (rng >> 20) % 84u, 1u + (rng >> 12) % 127u);
        if (blk % 4 == 1) rc_synth_note_off(&S, 24u + (rng >> 18) % 84u);
        if (blk % 500 == 7) rc_synth_seq_run(&S, (blk / 500) & 1);
        rc_synth_render(&S, buf, 441);
        for (i = 0; i < 441; i++) if (!rc_is_finite(buf[i]) || fabsf(buf[i]) > 8.0f) ok = 0;
    }
    CHECK(ok, "stress: finite and bounded output");
    printf("  stress (3000 blocks of random notes, parameters, sequencer): %s\n", ok ? "ok" : "FAILED");

    for (n = 0; n < 2; n++) {                   /* all parameters at their minimum, then their maximum */
        float pk = 0;
        fresh(&S);
        for (i = 0; i < RC_P_COUNT; i++) rc_synth_param127(&S, i, n ? 127u : 0u);
        rc_synth_note_on(&S, 48, 127); rc_synth_note_on(&S, 55, 127); rc_synth_note_on(&S, 59, 127); rc_synth_note_on(&S, 64, 127);
        for (blk = 0; blk < 6; blk++) {
            rc_synth_render(&S, buf, 44100);
            for (i = 0; i < 44100; i++) { if (!rc_is_finite(buf[i])) ok = 0; if (fabsf(buf[i]) > pk) pk = fabsf(buf[i]); }
        }
        printf("  all params %s: peak %.2f\n", n ? "max" : "min", (double)pk);
        CHECK(ok && pk < 8.0f, "extreme parameters finite/bounded");
    }
}

static void test_chunking(void)
{
    static float a[4410], b[4410];
    uint32_t pos = 0, k = 0, md;
    int i;
    double maxd = 0;
    static const uint32_t sizes[] = {1, 7, 16, 17, 31, 32, 33, 64, 100, 250};
    printf("[chunking]\n");
    fresh(&S);
    fresh(&S2);
    rc_synth_note_on(&S, 60, 100);
    rc_synth_note_on(&S2, 60, 100);
    rc_synth_render(&S, a, 4410);
    while (pos < 4410) {
        md = sizes[k++ % 10];
        if (md > 4410 - pos) md = 4410 - pos;
        rc_synth_render(&S2, b + pos, md);
        pos += md;
    }
    for (i = 0; i < 4410; i++) if (fabs((double)(a[i] - b[i])) > maxd) maxd = fabs((double)(a[i] - b[i]));
    printf("  max difference between chunkings: %.4f\n", maxd);
    CHECK(maxd < 0.2, "chunking changes the sound drastically");
}

int main(int argc, char **argv)
{
    test_math();
    test_svf();
    test_adsr();
    test_delay();
    test_lfsr();
    test_seq();
    test_synth(argc > 1 ? argv[1] : NULL);
    test_chunking();
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
