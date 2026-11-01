/* SPDX-License-Identifier: GPL-3.0-only */
/* BEAT CULT for the M-VAVE FM-1: three engines, PLVKS (a duophonic Formanta Polivoks-style voice), DUBMARINE (a dub siren) and DRUMS (sampled drums),
 * sharing a BBD echo, on the Dinghy platform (platform.c: updates, USB rescue, USB-MIDI, keys, knobs, screen, flash).
 *
 *   keys       play the voice (two notes at once: the newest two held notes sound)
 *   SELECT     the page (nine: filter, oscillator 1, oscillator 2, amp envelope, filter envelope, LFO, mod and drive, play, BBD echo)
 *   SEL EDIT ENV LFO GLO FX   jump to a section: filter / oscillators 1-2 / envelopes (amp, filter) / LFO (LFO, mod and drive) /
 *              play / BBD echo; press again to step through that section's pages (its button is lit)
 *   HOME       every value back to the starting sound
 *   KNOB 1-4   the four values of the page
 *   OCT- OCT+  the octave (kept in flash)
 *   PRESETS    the preset: F1..F12 (factory), U1..U16 (yours); loaded as you turn; * marks an edited sound
 *   SAVE       store the sound in the selected user slot (from a factory preset: the next free slot)
 *   ALGORITHM  the engine: PLVKS, DUBMARINE, DRUMS (one click, one engine)
 *   PLAY+ALGORITHM  hold PLAY and turn ALGORITHM: the MIDI channel, OMNI or 1..16 (kept in flash)
 *   MIDI       program change selects a preset
 *   MASTER     the volume (platform.c)
 *   DRUMS      keys 1-10 play the pads (OCT does not move them); MIDI notes are General MIDI drums (36 kick, 38 snare, 42/46 hats..).
 *              The pad grid shows hits; the PAD page tunes, shortens and levels the selected pad.
 *   SEQ        (DRUMS) the step sequencer. White keys are the 16 steps of the page, the selected pad's hits light up; tap a key:
 *              off, hit, accent. Black keys pick the pad. OCT- OCT+ move between pages of 16 steps (bars); SELECT turns the knob
 *              pages (TIMING: tempo, swing, length 16/32/48/64, rate 1/8 1/16 1/32; PATTERN: slot 1-16, sync, hit, accent).
 *              PLAY starts and stops (or MIDI Start/Stop/Clock when Sync is Ext). HOME clears the pattern, REC the pad's row,
 *              SAVE stores it; patterns also save themselves 2 s after an edit once stopped.
 *   MIDI in    notes, pitch bend, mod wheel (CC1), sustain (CC64), all-notes-off (CC123)
 *
 *   DUBMARINE  the dub siren (dubsiren.c, after beatcult/dubmarine) plays its pitch from the keys, with a glide for smooth
 *              sweeps; its pages are pitch and LFO, filter, play, echo. Its screen is a radar (ui_gfx.c draws it in integers).
 *
 * The voices (polivoks.c, dubsiren.c) and the echo (polivoks_fx.c) are float DSP: the build asks for the FM-1's float
 * hardware (-mfprev1, tools/build.py). tests/host.c runs this file on a computer. */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#define CV_MAX (240u * 128u)                  /* the canvas: the pad grid is 240 x 80, the radar 124 x 124 (platform.c: 240 x 64) */
#include "platform.c"
#include "ui_gfx.c"
#include "logo.h"
#ifdef DINGHY_HOST
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return 0; }   /* (no counter on the computer: the meter reads 0) */
#endif
#include "polivoks.c"
#include "polivoks_fx.c"
#include "dubsiren.c"
#include "drums.c"
#include "drumseq.c"
#include "sirenfmt.h"
#include "presets.h"

#define NVOICEPARAM POLIVOKS_NPARAMS                          /* 0..31 Polivoks */
#define SIREN_P NVOICEPARAM                                    /* 32..43 siren */
#define FX_P (SIREN_P + SIREN_NPARAMS)                         /* 44..47 echo (both engines) */
#define ENGINE_P (FX_P + POLIVOKS_FX_PARAMS)                   /* 48: 0 Polivoks, 1 siren */
#define PRESET_N (ENGINE_P + 1)                                /* what a preset holds: 0..48 (the engine is 0 PLVKS, 1 DUBMARINE, 2 DRUMS) */
#define PADP_P PRESET_N                                        /* 49..78: tune, decay, level of each pad (kept in the settings, not in presets) */
#define DSEL_P (PADP_P + DRUMS_PADVALS)                        /* 79: the pad the PAD page edits */
#define DLEV_P (DSEL_P + 1)                                    /* 80..83: drums level, crush, tone, choke */
#define SQ_P (DLEV_P + 4)                                      /* 84..91: the sequencer: tempo, swing, length, rate, pattern, sync, hit, accent (drumseq.h) */
#define NPARAM (SQ_P + SEQ_NPARAMS)
#define MAXPAGE 9

/* ---- the sound (audio ISR): events from the main loop, one writer each side */
enum { EV_NOTE, EV_PEDAL, EV_BEND, EV_MOD, EV_PARAM, EV_PANIC, EV_SEQ };
typedef struct { uint8_t type, a, b, pad; int16_t value; } event_t;
enum { RING = 128 };
static event_t ring[RING];
static volatile uint8_t ev_head, ev_tail, ready;
static uint32_t bank_store[POLIVOKS_BANK_BYTES / 4], siren_store[DUBSIREN_BANK_BYTES / 4], drums_store[DRUMS_BANK_BYTES / 4];
static volatile uint8_t engine;                 /* the engine that sounds: 0 PLVKS, 1 DUBMARINE, 2 DRUMS (the ISR's copy of ENGINE_P) */
static int32_t mono[POLIVOKS_FRAMES], left[POLIVOKS_FRAMES], right[POLIVOKS_FRAMES];
static volatile uint32_t dsp_peak;               /* the longest audio block since the main loop last looked, in us */
static uint8_t val[NPARAM];                     /* the values on the panel */
static uint32_t engine_val(void) { return val[ENGINE_P]; }

static void push(uint8_t type, uint8_t a, uint8_t b, int16_t value)
{
    uint8_t next = (uint8_t)((ev_head + 1u) % RING);
    if (next == ev_tail) return;                /* full: drop, never wait */
    ring[ev_head].type = type; ring[ev_head].a = a; ring[ev_head].b = b; ring[ev_head].value = value;
    ev_head = next;
}
static seq_t seq;                               /* the drum sequencer: its clock runs in the audio interrupt */
static uint8_t patbuf[2][SEQ_PAT_BYTES] __attribute__((aligned(4)));   /* the pattern the ISR plays, and one to load the next into */
static volatile uint8_t pat_live;
static void apply(unsigned p, unsigned v)       /* ISR side */
{
    if (p < NVOICEPARAM) polivoks_bank_set((polivoks_bank_t *)bank_store, p, v);
    else if (p < FX_P) dubsiren_set((dubsiren_bank_t *)siren_store, p - SIREN_P, v);
    else if (p < ENGINE_P) polivoks_fx_set(p - FX_P, v);
    else if (p == ENGINE_P) {
        if (v != engine) {                       /* another engine takes over: silence them all */
            polivoks_bank_panic((polivoks_bank_t *)bank_store);
            dubsiren_panic((dubsiren_bank_t *)siren_store);
            drums_panic((drums_bank_t *)drums_store);
            seq_kind(&seq, v == 2u);                 /* (stops it) */
            engine = (uint8_t)v;
        }
    }
    else if (p < DSEL_P) drums_set((drums_bank_t *)drums_store, p - PADP_P, v);
    else if (p > DSEL_P && p < SQ_P) drums_set((drums_bank_t *)drums_store, DRUMS_LEVEL + (p - DLEV_P), v);
    else if (p >= SQ_P) seq_set(&seq, p - SQ_P, v);
}

static void app_audio(int32_t *out, uint32_t n)
{
    polivoks_bank_t *bank = (polivoks_bank_t *)bank_store;
    dubsiren_bank_t *siren = (dubsiren_bank_t *)siren_store;
    drums_bank_t *drums = (drums_bank_t *)drums_store;
    uint32_t i, blk, t0 = fm1_ticks(), dt;
    if (!ready) return;
    while (ev_tail != ev_head) {
        const event_t *e = &ring[ev_tail];
        switch (e->type) {
        case EV_NOTE: if (engine == 2u) drums_note(drums, e->a, e->b); else if (engine) dubsiren_note(siren, e->a, e->b); else polivoks_bank_note(bank, e->a, e->b, 0); break;
        case EV_PEDAL: if (!engine) polivoks_bank_pedal(bank, e->a); break;
        case EV_BEND: if (engine == 1u) dubsiren_bend(siren, (int32_t)e->value * 16); else if (!engine) polivoks_bank_bend(bank, (int32_t)e->value * 16); break;   /* +-2 semitones */
        case EV_MOD: if (engine == 1u) dubsiren_mod(siren, e->a); else if (!engine) polivoks_bank_mod(bank, e->a); break;
        case EV_PARAM: apply(e->a, e->b); break;
        case EV_SEQ:
            if (e->a == SEQ_CLOCK) { if (engine != 1u) seq_clock_in(&seq, patbuf[pat_live]); }
            else if (e->a == SEQ_STOP || engine != 1u) seq_command(&seq, e->a);
            break;
        default: polivoks_bank_panic(bank); dubsiren_panic(siren); drums_panic(drums); break;
        }
        ev_tail = (uint8_t)((ev_tail + 1u) % RING);
    }
    for (blk = 0; blk + POLIVOKS_FRAMES <= n; blk += POLIVOKS_FRAMES) {
        for (i = 0; i < POLIVOKS_FRAMES; i++) mono[i] = 0;
        if (engine != 1u && seq.run) seq_block(&seq, patbuf[pat_live]);
        if (engine == 2u) drums_render(drums, mono); else if (engine) dubsiren_render(siren, mono); else polivoks_bank_render(bank, mono);
        for (i = 0; i < POLIVOKS_FRAMES; i++) left[i] = right[i] = mono[i];
        polivoks_fx_process(mono, left, right);
        for (i = 0; i < POLIVOKS_FRAMES; i++) {   /* Q23 -> Q15 */
            out[2u * (blk + i)] += left[i] >> 8;
            out[2u * (blk + i) + 1u] += right[i] >> 8;
        }
    }
    dt = (uint32_t)(fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (dt > dsp_peak) dsp_peak = dt;
}

/* ---- float self-test: a fixed note through a scratch voice bank; the sum of the output must match the computer's */
static char self_msg[40];
static uint32_t scratch[POLIVOKS_BANK_BYTES / 4], test_out[POLIVOKS_FRAMES];
static uint32_t selftest(void)
{
    polivoks_bank_t *b = (polivoks_bank_t *)scratch;
    int32_t *o = (int32_t *)test_out;
    uint32_t sum = 0, blk, i;
    polivoks_bank_init(b);
    polivoks_bank_note(b, 57, 100, 0);
    for (blk = 0; blk < 40u; blk++) {
        for (i = 0; i < POLIVOKS_FRAMES; i++) o[i] = 0;
        polivoks_bank_render(b, o);
        for (i = 0; i < POLIVOKS_FRAMES; i++) sum += (uint32_t)((o[i] < 0 ? -o[i] : o[i]) >> 10);
    }
    return sum;
}
static volatile uint32_t selftest_expect = 4571585u;   /* (not a constant in the code: -4571585 would look like a ROM address to the build check) */
#define SELFTEST_EXPECT selftest_expect                        /* what the computer gets (host run prints it) */

/* ---- parameters: names, ranges, defaults */
static const fm1_param_t FXP[POLIVOKS_FX_PARAMS] = {
    {"Time", 127, 0, 0}, {"Feedbk", 127, 0, 0}, {"Tone", 127, 0, 0}, {"Echo", 127, 0, 0}};
static const char *const ENGINE_NAMES[] = {"PLVKS", "DUBMARINE", "DRUMS"};
static const fm1_param_t ENGP = {"Engine", 2, 0, ENGINE_NAMES};
static const char *const CHOKE_NAMES[] = {"Off", "On"};
static const fm1_param_t PADP[DRUM_PADPARAMS] = {{"Tune", 127, 64, 0}, {"Decay", 127, 127, 0}, {"Level", 127, 100, 0}};
static const char *const LEN_NAMES[] = {"16", "32", "48", "64"}, *const RATE_NAMES[] = {"1/8", "1/16", "1/32"}, *const SYNC_NAMES[] = {"Int", "Ext"};
static const fm1_param_t SQP[SEQ_NPARAMS] = {{"Tempo", 200, 80, 0}, {"Swing", 127, 0, 0}, {"Length", 3, 0, LEN_NAMES}, {"Rate", 2, 1, RATE_NAMES},
                                             {"Pattern", 15, 0, 0}, {"Sync", 1, 0, SYNC_NAMES}, {"Hit", 127, 90, 0}, {"Accent", 127, 127, 0}};
static const fm1_param_t dselp = {"Pad", DRUMS_NPAD - 1, 0, DRUM_ELKA_NAMES};
static const fm1_param_t DGLOBP[4] = {{"Level", 127, 100, 0}, {"Crush", 127, 0, 0}, {"Tone", 127, 127, 0}, {"Choke", 1, 1, CHOKE_NAMES}};
static const fm1_param_t *param(unsigned p)
{
    return p < NVOICEPARAM ? polivoks_param((int)p) : p < FX_P ? dubsiren_param((int)(p - SIREN_P))
         : p < ENGINE_P ? &FXP[p - FX_P] : p == ENGINE_P ? &ENGP : p < DSEL_P ? &PADP[(p - PADP_P) % DRUM_PADPARAMS]
         : p == DSEL_P ? &dselp : p < SQ_P ? &DGLOBP[p - DLEV_P] : &SQP[p - SQ_P];
}
/* pages: the first parameter of each (four to a page), per engine */
static const uint8_t PV_PAGE[9] = {0, 4, 8, 12, 16, 20, 24, 28, FX_P};
static const char *const PV_TITLE[9] = {"FILTER", "OSCILLATOR 1", "OSCILLATOR 2", "AMP ENVELOPE", "FILTER ENV",
                                        "LFO", "MOD AND DRIVE", "PLAY", "BBD ECHO"};
static const uint8_t SI_PAGE[4] = {SIREN_P, SIREN_P + 4, SIREN_P + 8, FX_P};
static const char *const SI_TITLE[4] = {"PITCH LFO", "FILTER", "PLAY", "BBD ECHO"};
static const char *const DR_TITLE[3] = {"PAD", "MIX", "BBD ECHO"}, *const SQ_TITLE[2] = {"TIMING", "PATTERN"};
static int32_t stepedit = -1;                    /* SEQ: the step whose notes or hit the four knobs edit (hold a step, press EDIT), or -1 */
static uint8_t seqmode, seqview;                 /* SEQ mode (DRUMS only) and the page of 16 steps on show */
static uint32_t seqpg;                           /* the knob page in SEQ mode */
static uint32_t npage(void) { return seqmode ? 2u : engine_val() == 2u ? 3u : engine_val() ? 4u : 9u; }
static uint32_t page_base(uint32_t page) { return engine_val() ? SI_PAGE[page] : PV_PAGE[page]; }
static const char *page_title(uint32_t page) { return seqmode ? SQ_TITLE[page] : engine_val() == 2u ? DR_TITLE[page] : engine_val() ? SI_TITLE[page] : PV_TITLE[page]; }
static uint32_t page_param(uint32_t page, uint32_t k)       /* the k-th parameter on the page (the PAD page edits the selected pad) */
{
    if (seqmode) return SQ_P + page * 4u + k;
    if (engine_val() == 2u) return page == 0 ? (k == 0 ? DSEL_P : PADP_P + val[DSEL_P] * DRUM_PADPARAMS + k - 1u) : page == 1 ? DLEV_P + k : FX_P + k;
    return page_base(page) + k;
}

static void set_param(unsigned p, unsigned v)   /* main loop */
{
    const fm1_param_t *d = param(p);
    if (v > d->max) v = d->max;
    val[p] = (uint8_t)v;
    push(EV_PARAM, (uint8_t)p, (uint8_t)v, 0);
}
static void defaults(void)
{
    unsigned p;
    for (p = 0; p < NVOICEPARAM; p++) val[p] = polivoks_param((int)p)->def;
    for (p = 0; p < SIREN_NPARAMS; p++) val[SIREN_P + p] = dubsiren_param((int)p)->def;
    for (p = 0; p < POLIVOKS_FX_PARAMS; p++) val[FX_P + p] = polivoks_fx_default[p];
    val[ENGINE_P] = 0;
    for (p = PADP_P; p < SQ_P; p++) val[p] = param(p)->def;
}

/* ---- presets: NFACTORY in the code, NUSER in flash (blob 0, one sector: saved on SAVE) */
#define NUSER 16u
#define NPRESET (NFACTORY + NUSER)
static struct { uint8_t used[NUSER]; uint8_t p[NUSER][PRESET_N]; } ubank;
static struct { uint8_t used[NUSER]; uint8_t p[NUSER][36]; } old_bank;   /* the layout of the first preset build */
static volatile uint32_t cur, modified, ui_reset;
static char msg[40];
static uint32_t msg_n;
static uint32_t msg_until;

static void load_preset(uint32_t i)             /* main loop, or MIDI program change */
{
    unsigned p;
    if (i >= NPRESET) return;
    if (i < NFACTORY) for (p = 0; p < PRESET_N; p++) val[p] = FACTORY[i][p];
    else if (ubank.used[i - NFACTORY]) for (p = 0; p < PRESET_N; p++) val[p] = ubank.p[i - NFACTORY][p];
    else defaults();
    for (p = 0; p < NPARAM; p++) {
        if (val[p] > param(p)->max) val[p] = param(p)->max;
        push(EV_PARAM, (uint8_t)p, val[p], 0);
    }
    cur = i; modified = 0; ui_reset = 1; msg_until = 0;
    if (val[ENGINE_P] == 1u) seqmode = 0;       /* (the siren has no sequencer) */
    stepedit = -1;
}
static void say(const char *s, uint32_t ms)
{
    uint32_t n = 0;
    while (s[n] && n < 39u) { msg[n] = s[n]; n++; }
    msg[n] = 0;
    msg_n++;
    msg_until = fm1_ms + ms;
}
static void save_preset(void)                   /* SAVE: into the selected user slot, or the next free one */
{
    uint32_t slot = NUSER, k;
    char t[16] = "SAVED U";
    if (cur >= NFACTORY) slot = cur - NFACTORY;
    else for (k = 0; k < NUSER; k++) if (!ubank.used[k]) { slot = k; break; }
    if (slot >= NUSER) { say("BANK FULL", 1500); return; }
    for (k = 0; k < PRESET_N; k++) ubank.p[slot][k] = val[k];
    ubank.used[slot] = 1;
    if (blob_save(0, &ubank, sizeof ubank)) { say("SAVE FAILED", 2000); return; }
    cur = NFACTORY + slot; modified = 0;
    t[7] = (char)('0' + (slot + 1u) / 10u); t[8] = (char)('0' + (slot + 1u) % 10u); t[9] = 0;
    say(t, 1500);
}

/* ---- patterns: NPAT slots a bank, the drums' in flash slot 0 and PLVKS's in slot 1 (4 sectors each); the working one is patbuf[pat_live] */
#define NPAT 16u
typedef struct { uint8_t tempo, swing, len, rate, hit, acc, r0, r1; uint8_t d[SEQ_PAT_BYTES]; } patrec_t;
static struct { uint8_t used[NPAT]; patrec_t p[NPAT]; } pbank;
static struct { uint8_t used[NPAT]; struct { uint8_t tempo, swing, len, rate, hit, acc, r0, r1; uint8_t st[SEQ_DR_BYTES]; } p[NPAT]; } old_pbank;   /* blob 1 of the first sequencer build */
static uint32_t pat_dirty, pat_t, pat_ver, bank_kind = 255u;
static uint32_t cur_pat(void) { return val[SQ_P + SEQ_PATTERN]; }
static uint32_t bank_slot(void) { return bank_kind == SEQ_KIND_DRUMS ? 0u : 1u; }
static void pattern_store(uint32_t slot)
{
    uint32_t k;
    const uint8_t *st = patbuf[pat_live];
    if (bank_kind == 255u || slot >= NPAT) return;
    pbank.p[slot].tempo = val[SQ_P + SEQ_TEMPO]; pbank.p[slot].swing = val[SQ_P + SEQ_SWING]; pbank.p[slot].len = val[SQ_P + SEQ_LEN];
    pbank.p[slot].rate = val[SQ_P + SEQ_RATE]; pbank.p[slot].hit = val[SQ_P + SEQ_HIT]; pbank.p[slot].acc = val[SQ_P + SEQ_ACC];
    for (k = 0; k < SEQ_PAT_BYTES; k++) pbank.p[slot].d[k] = st[k];
    pbank.used[slot] = 1;
    pat_dirty = 0;
    if (slot_save(bank_slot(), &pbank, sizeof pbank)) say("SAVE FAILED", 2000);
}
static void set_param(unsigned p, unsigned v);
static void pattern_load(uint32_t slot)
{
    uint32_t k, nx = 1u - pat_live;
    if (slot >= NPAT) return;
    if (pbank.used[slot]) {
        for (k = 0; k < SEQ_PAT_BYTES; k++) patbuf[nx][k] = pbank.p[slot].d[k];
        set_param(SQ_P + SEQ_TEMPO, pbank.p[slot].tempo); set_param(SQ_P + SEQ_SWING, pbank.p[slot].swing);
        set_param(SQ_P + SEQ_LEN, pbank.p[slot].len); set_param(SQ_P + SEQ_RATE, pbank.p[slot].rate);
        set_param(SQ_P + SEQ_HIT, pbank.p[slot].hit); set_param(SQ_P + SEQ_ACC, pbank.p[slot].acc);
    } else seq_clear(patbuf[nx]);                                        /* an empty slot: no steps, the same tempo */
    pat_live = (uint8_t)nx;
    pat_dirty = 0; pat_ver++;
}
static void ensure_bank(void)                   /* the bank of the engine that is up: the drums' or PLVKS's */
{
    uint32_t kind = engine_val() == 2u ? SEQ_KIND_DRUMS : SEQ_KIND_PLVKS, k, u;
    uint8_t *r = (uint8_t *)&pbank;
    if (engine_val() == 1u || bank_kind == kind) return;
    if (bank_kind != 255u && pat_dirty) pattern_store(cur_pat());
    bank_kind = kind;
    for (k = 0; k < sizeof pbank; k++) r[k] = 0;
    if (slot_load(bank_slot(), &pbank, sizeof pbank)) {
        for (k = 0; k < sizeof pbank; k++) r[k] = 0;
        if (kind == SEQ_KIND_DRUMS && !blob_load(1, &old_pbank, sizeof old_pbank)) {     /* the first sequencer build kept its drum patterns in blob 1 */
            for (u = 0; u < NPAT; u++) if (old_pbank.used[u]) {
                for (k = 0; k < 8u; k++) ((uint8_t *)&pbank.p[u])[k] = ((uint8_t *)&old_pbank.p[u])[k];
                for (k = 0; k < SEQ_DR_BYTES; k++) pbank.p[u].d[k] = old_pbank.p[u].st[k];
                pbank.used[u] = 1;
            }
            slot_save(0, &pbank, sizeof pbank);
        }
    }
    pattern_load(cur_pat());
}
static uint32_t last_step_note = 48;            /* the note a new PLVKS step starts with: the last one edited */
static uint32_t aud_n[2] = {255, 255}, aud_t;   /* an audition: the notes sounding, and since when */
static void play(uint32_t note, uint32_t vel);
static void stop(uint32_t note);
static void audition(uint32_t n1, uint32_t n2, uint32_t vel)
{
    if (aud_n[0] != 255u) stop(aud_n[0]);
    if (aud_n[1] != 255u) stop(aud_n[1]);
    aud_n[0] = n1; aud_n[1] = n2; aud_t = fm1_ms;
    play(n1, vel);
    if (n2 != 255u) play(n2, vel);
}
static void touch_pat(void) { pat_dirty = 1; pat_t = fm1_ms; pat_ver++; }
static uint32_t step_tap(uint32_t step)         /* a tap on a step: drums off / hit / accent (and its edits go); PLVKS on / off */
{
    uint8_t *pat = patbuf[pat_live];
    uint32_t on;
    if (bank_kind == SEQ_KIND_DRUMS) {
        uint32_t pd = val[DSEL_P], n = (seq_get(pat, step, pd) + 1u) % 3u;
        seq_hit_del(pat, step, pd);
        seq_put(pat, step, pd, n);
        on = n;
        if (n) audition(drums_note_of_pad(pd), 255u, n == 2u ? val[SQ_P + SEQ_ACC] : val[SQ_P + SEQ_HIT]);
    } else {
        seq_note_t r;
        seq_note_get(pat, step, &r);
        if (r.on) r.on = 0;
        else {
            r.on = 1;
            if (!r.amp) { r.n1 = (uint8_t)last_step_note; r.has2 = 0; r.n2 = 0; r.len = 12; r.delay = 0; r.amp = (uint8_t)val[SQ_P + SEQ_HIT]; }
        }
        seq_note_put(pat, step, &r);
        on = r.on;
        if (on) audition(r.n1, r.has2 ? r.n2 : 255u, r.amp);
    }
    touch_pat();
    return on;
}
static void edit_enter(uint32_t step)           /* hold a step, press EDIT: its knobs. An empty step is switched on first */
{
    uint8_t *pat = patbuf[pat_live];
    if (bank_kind == SEQ_KIND_DRUMS) {
        if (!seq_get(pat, step, val[DSEL_P])) { seq_put(pat, step, val[DSEL_P], 1); touch_pat(); }
    } else {
        seq_note_t r;
        seq_note_get(pat, step, &r);
        if (!r.on) step_tap(step);
    }
    stepedit = (int32_t)step;
    pat_ver++;
}
/* the four values of the edited step. drums: pitch (half semitones), velocity, delay, ratchet. PLVKS: note 1, note 2 (0: none), length, velocity */
static void sf_get(uint32_t k, int32_t *v, int32_t *mn, int32_t *mx)
{
    const uint8_t *pat = patbuf[pat_live];
    uint32_t step = (uint32_t)stepedit;
    if (bank_kind == SEQ_KIND_DRUMS) {
        seq_hit_t h;
        uint32_t pd = val[DSEL_P], st = seq_get(pat, step, pd);
        if (!seq_hit_get(pat, step, pd, &h)) { h.tune = 24; h.vel = (uint8_t)(st == 2u ? val[SQ_P + SEQ_ACC] : val[SQ_P + SEQ_HIT]); h.delay = 0; h.ratchet = 0; }
        *v = k == 0 ? h.tune : k == 1u ? h.vel : k == 2u ? h.delay : h.ratchet;
        *mn = k == 1u ? 1 : 0; *mx = k == 0 ? 48 : k == 1u ? 127 : k == 2u ? 15 : 3;
    } else {
        seq_note_t r;
        seq_note_get(pat, step, &r);
        *v = k == 0 ? r.n1 : k == 1u ? (r.has2 ? (int32_t)r.n2 + 1 : 0) : k == 2u ? r.len : r.amp;
        *mn = k == 3u ? 1 : 0; *mx = k == 0 ? 127 : k == 1u ? 128 : k == 2u ? 15 : 127;
    }
}
static void sf_set(uint32_t k, int32_t v)
{
    uint8_t *pat = patbuf[pat_live];
    uint32_t step = (uint32_t)stepedit;
    if (bank_kind == SEQ_KIND_DRUMS) {
        seq_hit_t h;
        uint32_t pd = val[DSEL_P], st = seq_get(pat, step, pd);
        if (!st) seq_put(pat, step, pd, st = 1u);
        if (!seq_hit_get(pat, step, pd, &h)) { h.tune = 24; h.vel = (uint8_t)(st == 2u ? val[SQ_P + SEQ_ACC] : val[SQ_P + SEQ_HIT]); h.delay = 0; h.ratchet = 0; }
        if (k == 0) h.tune = (uint8_t)v; else if (k == 1u) h.vel = (uint8_t)v; else if (k == 2u) h.delay = (uint8_t)v; else h.ratchet = (uint8_t)v;
        if (seq_hit_put(pat, step, pd, &h)) say("STEP EDITS FULL", 1500);
        audition(drums_note_of_pad(pd), 255u, h.vel);
    } else {
        seq_note_t r;
        seq_note_get(pat, step, &r);
        r.on = 1;
        if (k == 0) { r.n1 = (uint8_t)v; last_step_note = (uint32_t)v; }
        else if (k == 1u) { r.has2 = v > 0; r.n2 = (uint8_t)(v > 0 ? v - 1 : 0); }
        else if (k == 2u) r.len = (uint8_t)v; else r.amp = (uint8_t)v;
        seq_note_put(pat, step, &r);
        audition(r.n1, r.has2 ? r.n2 : 255u, r.amp);
    }
    touch_pat();
}
static void copy_bar(void)                      /* HOME + ARP: the 16 steps on show (and their edits) into the next page; the pattern grows by one if it ends here */
{
    uint32_t pages = val[SQ_P + SEQ_LEN] + 1u;
    if ((uint32_t)seqview + 1u >= pages) {
        if (pages >= 4u) { say("THIS IS THE LAST BAR", 1500); return; }
        set_param(SQ_P + SEQ_LEN, pages);       /* one bar longer */
    }
    seq_copy_page(patbuf[pat_live], bank_kind, seqview, (uint32_t)seqview + 1u);
    seqview++;
    touch_pat();
    say(seqview == 1u ? "BAR 1 COPIED TO BAR 2" : seqview == 2u ? "BAR 2 COPIED TO BAR 3" : "BAR 3 COPIED TO BAR 4", 1500);
}

/* ---- notes (main loop) */
static uint32_t last_note = 255;
static uint32_t nheld[4], ping;                 /* the held notes (a bit each) and the radar's ping (0 = none) */
static uint8_t padhit[DRUMS_NPAD];              /* DRUMS: how brightly each pad shows its last hit (8 = just hit) */
static void play(uint32_t note, uint32_t vel)
{
    if (note > 127u || !vel) return;
    push(EV_NOTE, (uint8_t)note, (uint8_t)vel, 0);
    last_note = note;
    nheld[note >> 5] |= 1u << (note & 31u);
    ping = 1;
    if (engine_val() == 2u) { int pd = drums_pad_of_note(note); if (pd >= 0) padhit[pd] = 8; }
}
static void stop(uint32_t note)
{
    if (note > 127u) return;
    push(EV_NOTE, (uint8_t)note, 0, 0);
    nheld[note >> 5] &= ~(1u << (note & 31u));
}
static uint32_t gate(void) { return nheld[0] | nheld[1] | nheld[2] | nheld[3]; }

/* ---- the screen */
#define C_ACC RGB(255, 150, 60)                 /* the Polivoks' amber */
#define C_GREY RGB(90, 90, 90)
#define C_TRACK RGB(40, 40, 40)
#define KEY0 53u                                /* key 0 = F3 */

static void put_big(char *s, uint32_t v)
{
    char r[11]; uint32_t n = 0, i = 0;
    do { r[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) s[i++] = r[--n];
    s[i] = 0;
}
static void put_num(char *s, uint32_t v)
{
    uint32_t i = 0;
    if (v >= 100u) s[i++] = (char)('0' + v / 100u);
    if (v >= 10u) s[i++] = (char)('0' + v / 10u % 10u);
    s[i++] = (char)('0' + v % 10u);
    s[i] = 0;
}
static void put_dec(char *s, uint32_t whole, uint32_t tenths_or_hund, uint32_t digits)   /* whole.fraction */
{
    uint32_t i = 0, d;
    char r[8]; uint32_t n = 0;
    do { r[n++] = (char)('0' + whole % 10u); whole /= 10u; } while (whole);
    while (n) s[i++] = r[--n];
    s[i++] = '.';
    for (d = digits; d; d--) { uint32_t div = d == 2u ? 10u : 1u; s[i++] = (char)('0' + tenths_or_hund / div % 10u); }
    s[i] = 0;
}
static void put_unit(char *s, const char *u)
{
    while (*s) s++;
    while (*u) *s++ = *u++;
    *s = 0;
}
static void format_param(unsigned p, char *s)     /* the siren's values in their own units (as dubmarine shows them) */
{
    uint32_t v = val[p], x;
    if (p >= SQ_P) {
        switch (p - SQ_P) {
        case SEQ_TEMPO: put_num(s, 40u + v); return;
        case SEQ_SWING: put_num(s, 50u + v * 25u / 127u); put_unit(s, "%"); return;
        case SEQ_PATTERN: put_num(s, v + 1u); return;
        default: break;
        }
    }
    if (p >= PADP_P && p < DSEL_P) {
        uint32_t k = (p - PADP_P) % DRUM_PADPARAMS;
        if (k == DRUM_TUNE) {                                   /* semitones, +-12 */
            x = (v > 64u ? v - 64u : 64u - v) * 120u / 64u;
            s[0] = v >= 64u ? '+' : '-'; put_dec(s + 1, x / 10u, x % 10u, 1); put_unit(s, " st"); return;
        }
        if (k == DRUM_DECAY && v >= 127u) { s[0] = 'F'; s[1] = 'U'; s[2] = 'L'; s[3] = 'L'; s[4] = 0; return; }
        put_num(s, v); return;
    }
    if (p < SIREN_P || p >= FX_P) { if (param(p)->names) { const char *n = param(p)->names[v]; uint32_t i = 0; while (n[i] && i < 15u) { s[i] = n[i]; i++; } s[i] = 0; } else put_num(s, v); return; }
    switch (p - SIREN_P) {
    case SIREN_TUNE: case SIREN_CUTOFF:
        x = (p - SIREN_P == SIREN_TUNE) ? FMT_TUNE_HZ[v] : FMT_CUT_HZ[v];
        if (x >= 1000u) { put_dec(s, x / 1000u, x / 100u % 10u, 1); put_unit(s, "k"); } else { put_num(s, x); put_unit(s, " Hz"); }
        return;
    case SIREN_RATE: x = FMT_RATE_X10[v]; put_dec(s, x / 10u, x % 10u, 1); put_unit(s, " Hz"); return;
    case SIREN_DEPTH: put_num(s, v * 100u / 127u); put_unit(s, "%"); return;
    case SIREN_RESO: x = 5u + v * 195u / 127u; put_dec(s, x / 10u, x % 10u, 1); return;
    case SIREN_SWEEP:
        if (v >= 62u && v <= 66u) { s[0] = 'F'; s[1] = 'L'; s[2] = 'A'; s[3] = 'T'; s[4] = 0; return; }
        s[0] = v > 64u ? 'U' : 'D'; s[1] = v > 64u ? 'P' : 'N'; s[2] = ' '; s[3] = v > 64u ? '+' : '-';
        put_num(s + 4, (v > 64u ? v - 64u : 64u - v) * 100u / 63u); put_unit(s, "%"); return;
    case SIREN_DECAY: x = 100u + 5900u * v / 127u; put_dec(s, x / 1000u, x / 10u % 100u, 2); put_unit(s, " s"); return;
    case SIREN_GLIDE:
        x = FMT_GLIDE_MS[v];
        if (x >= 1000u) { put_dec(s, x / 1000u, x / 100u % 10u, 1); put_unit(s, " s"); } else { put_num(s, x); put_unit(s, " ms"); }
        return;
    case SIREN_VOLUME: x = v * 100u / 127u; put_dec(s, x / 10u, x % 10u, 1); return;
    default: if (param(p)->names) { const char *n = param(p)->names[v]; uint32_t i = 0; while (n[i] && i < 15u) { s[i] = n[i]; i++; } s[i] = 0; } else put_num(s, v); return;
    }
}
/* PLVKS: a panel in the Polivoks' manner: cream legends, black knobs with a white pointer, rocker switches, a red lamp */
#define C_CREAM RGB(236, 226, 196)
#define C_CREAM_DIM RGB(150, 142, 120)
#define C_LEGEND RGB(80, 75, 64)
#define C_RED RGB(255, 40, 30)
/* DUBMARINE: a radar's phosphor greens */
#define C_PH0 RGB(0, 18, 8)
#define C_PH1 RGB(0, 70, 30)
#define C_PH2 RGB(20, 150, 64)
#define C_PH3 RGB(60, 215, 100)
#define C_PH4 RGB(150, 255, 175)
#define KNOB_Y 66
#define RADAR_X 4
#define RADAR_Y 62
#define RADAR_R 60

static void draw_knob(unsigned col, unsigned p)
{
    const fm1_param_t *d = param(p);
    char s[24];
    uint32_t v = val[p], mx = d->max, n, i;
    int32_t cx4 = 30 * 4, cy4 = 58 * 4, a, w;
    format_param(p, s);
    cv_begin(60, 112, C_BLACK);
    w = text_w(&FONT_S, d->name);
    cv_text((60 - w) / 2, 0, &FONT_S, d->name, C_CREAM_DIM);
    if (mx == 1u) {                                              /* a switch: a rocker */
        cv_rect(18, 38, 24, 40, RGB(96, 96, 100));
        cv_rect(20, 40, 20, 36, RGB(14, 14, 16));
        if (v) { cv_rect(21, 41, 18, 17, C_CREAM); cv_rect(21, 59, 18, 16, RGB(40, 40, 44)); }
        else { cv_rect(21, 41, 18, 17, RGB(40, 40, 44)); cv_rect(21, 59, 18, 16, C_CREAM); }
        cv_rect(21, 57, 18, 2, RGB(70, 70, 74));
    } else {
        n = mx > 8u ? 10u : mx;
        for (i = 0; i <= n; i++) {                               /* the printed scale */
            int32_t ang = -135 + (int32_t)(i * 270u / n);
            int32_t tx = cx4 + ((104 * ui_sin(ang)) >> 14), ty = cy4 - ((104 * ui_cos(ang)) >> 14);
            int lit = mx > 8u ? (i * mx <= v * 10u) : (i == v);
            ui_disc(tx, ty, lit ? 7 : 5, lit ? C_ACC : RGB(64, 60, 52));
        }
        ui_disc(cx4, cy4, 80, RGB(24, 24, 26));                  /* the knob */
        ui_ring(cx4, cy4, 78, 3, RGB(110, 108, 104));
        ui_disc(cx4, cy4, 56, RGB(38, 38, 40));
        a = -135 + (int32_t)(v * 270u / (mx ? mx : 1u));
        ui_spoke(cx4, cy4, a, 5, 17, 7, C_CREAM);                /* the pointer */
    }
    w = text_w(&FONT_S, s);
    cv_text((60 - w) / 2, 92, &FONT_S, s, C_WHITE);
    cv_rect(4, 110, 52, 1, C_LEGEND);                             /* the legend's rule */
    cv_blit(col * 60u, KNOB_Y);
}
static void draw_readout(unsigned row, unsigned p)               /* DUBMARINE: the radar's side column */
{
    const fm1_param_t *d = param(p);
    char s[24];
    format_param(p, s);
    cv_begin(106, 32, C_BLACK);
    cv_text(0, 0, &FONT_S, d->name, C_PH2);
    cv_text(0, 16, &FONT_S, s, C_PH4);
    cv_blit(134, RADAR_Y + row * 34u);
}
/* DRUMS: a grid of pads (a hit lights its pad, the selected pad has a white frame) and four values under it */
#define C_DR RGB(255, 150, 60)
#define C_DRD RGB(110, 60, 20)
#define C_DRL RGB(190, 110, 50)
static void draw_cell(unsigned col, unsigned p)
{
    const fm1_param_t *d = param(p);
    char s[24];
    int32_t w;
    format_param(p, s);
    cv_begin(60, 34, C_BLACK);
    w = text_w(&FONT_S, d->name);
    cv_text((60 - w) / 2, 0, &FONT_S, d->name, C_DRL);
    w = text_w(&FONT_S, s);
    cv_text((60 - w) / 2, 17, &FONT_S, s, C_WHITE);
    cv_blit(col * 60u, seqmode ? 164u : 148u);
}
static void draw_pads(void)
{
    uint32_t i;
    cv_begin(240, 80, C_BLACK);
    for (i = 0; i < DRUMS_NPAD; i++) {
        int32_t x = 4 + (int32_t)(i % 5u) * 48, y = (int32_t)(i / 5u) * 40, w;
        uint32_t lv = (uint32_t)padhit[i] * 2u;                  /* 0..16 */
        uint16_t fill = ui_mix(C_DR, RGB(22, 12, 4), lv), edge = i == val[DSEL_P] ? C_WHITE : ui_mix(C_DR, C_DRD, lv);
        cv_rect(x, y, 44, 36, edge);
        cv_rect(x + 2, y + 2, 40, 32, fill);
        w = text_w(&FONT_S, drums_names[i]);
        cv_text(x + (44 - w) / 2, y + 10, &FONT_S, drums_names[i], lv > 8u ? C_BLACK : C_DR);
    }
    cv_blit(0, 62);
}
static const int8_t KEY_STEP[27] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};   /* the white keys: 16 steps */
static const int8_t KEY_PAD[27] = {-1, 0, -1, 1, -1, 2, -1, -1, 3, -1, 4, -1, -1, 5, -1, 6, -1, 7, -1, -1, 8, -1, 9, -1, -1, 10, -1};     /* the black keys: the pads */
static void frame_col(uint32_t c);
static void draw_stepgrid(void)                                  /* SEQ: ten pads by sixteen steps of the page; the selected pad's row is lit */
{
    uint32_t r, c, run = seq.run, pos = seq.pos;
    const uint8_t *st = patbuf[pat_live];
    cv_begin(240, 100, C_BLACK);
    for (r = 0; r < DRUMS_NPAD; r++)
        for (c = 0; c < 16u; c++) {
            uint32_t step = (uint32_t)seqview * 16u + c, sv = seq_get(st, step, r);
            uint16_t col = sv == 2u ? C_WHITE : sv == 1u ? C_DR : r == val[DSEL_P] ? ((c >> 2) & 1u ? RGB(70, 42, 16) : RGB(58, 34, 12)) : ((c >> 2) & 1u ? RGB(32, 19, 8) : RGB(22, 13, 5));
            if (run && step == pos) col = sv ? RGB(255, 235, 170) : RGB(130, 90, 44);
            cv_rect((int32_t)c * 15, (int32_t)r * 10, 13, 8, col);
        }
    if (stepedit >= 0 && (uint32_t)stepedit / 16u == seqview) frame_col((uint32_t)stepedit % 16u);
    cv_blit(0, 60);
}
static void note_str(char *s, uint32_t n)       /* C4 (60), C#4 ... */
{
    static const char *const NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const char *q = NM[n % 12u];
    uint32_t o = 0;
    while (*q) s[o++] = *q++;
    if (n / 12u < 1u) { s[o++] = '-'; s[o++] = '1'; } else put_num(s + o, n / 12u - 1u);
    if (n / 12u >= 1u) { while (s[o]) o++; }
    s[o] = 0;
}
static void draw_cell_txt(unsigned col, const char *name, const char *text)
{
    cv_begin(60, 34, C_BLACK);
    cv_text((60 - text_w(&FONT_S, name)) / 2, 0, &FONT_S, name, C_DRL);
    cv_text((60 - text_w(&FONT_S, text)) / 2, 17, &FONT_S, text, C_WHITE);
    cv_blit(col * 60u, 164u);
}
static void sf_draw(uint32_t k, int32_t v)      /* one of the edited step's four values */
{
    static const char *const DN[4] = {"PITCH", "LEVEL", "DELAY", "REPEAT"}, *const PN[4] = {"NOTE 1", "NOTE 2", "LENGTH", "LEVEL"};
    char t[16];
    uint32_t o = 0;
    if (bank_kind == SEQ_KIND_DRUMS) {
        if (k == 0) {
            int32_t h = v - 24;
            if (h < 0) { t[o++] = '-'; h = -h; } else if (h > 0) t[o++] = '+';
            put_num(t + o, (uint32_t)h / 2u); while (t[o]) o++;
            if (h & 1) { t[o++] = '.'; t[o++] = '5'; }
            t[o] = 0;
        } else if (k == 1u) put_num(t, (uint32_t)v);
        else if (k == 2u) { put_num(t, (uint32_t)v * 100u / 32u); while (t[o]) o++; t[o++] = '%'; t[o] = 0; }
        else { t[o++] = 'x'; t[o++] = (char)('1' + v); t[o] = 0; }
        draw_cell_txt(k, DN[k], t);
    } else {
        if (k == 0) note_str(t, (uint32_t)v);
        else if (k == 1u) { if (!v) { t[0] = 'O'; t[1] = 'F'; t[2] = 'F'; t[3] = 0; } else note_str(t, (uint32_t)v - 1u); }
        else if (k == 2u) { put_num(t, ((uint32_t)v + 1u) * 100u / 16u); while (t[o]) o++; t[o++] = '%'; t[o] = 0; }
        else put_num(t, (uint32_t)v);
        draw_cell_txt(k, PN[k], t);
    }
}
static void frame_col(uint32_t c)               /* the edited step: a white frame round its column */
{
    int32_t x = (int32_t)c * 15 - 1;
    if (x < 0) x = 0;
    cv_rect(x, 0, 15, 1, C_WHITE); cv_rect(x, 99, 15, 1, C_WHITE);
    cv_rect(x, 0, 1, 100, C_WHITE); cv_rect(x + 14, 0, 1, 100, C_WHITE);
}
static void draw_notegrid(void)                 /* SEQ on PLVKS: twelve note names by sixteen steps; the octave is the colour, the length the width */
{
    uint32_t c, run = seq.run, pos = seq.pos;
    const uint8_t *st = patbuf[pat_live];
    cv_begin(240, 100, C_BLACK);
    for (c = 0; c < 16u; c++) {
        uint32_t step = (uint32_t)seqview * 16u + c, r;
        seq_note_t n;
        seq_note_get(st, step, &n);
        for (r = 0; r < 12u; r++) {
            static const uint8_t BLK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
            uint16_t col = BLK[11u - r] ? ((c >> 2) & 1u ? RGB(34, 20, 8) : RGB(26, 15, 6)) : ((c >> 2) & 1u ? RGB(52, 31, 12) : RGB(42, 25, 9));
            if (run && step == pos) col = RGB(90, 58, 24);
            cv_rect((int32_t)c * 15, (int32_t)r * 8, 13, 7, col);
        }
        if (n.on) {
            uint32_t i;
            for (i = 0; i < (n.has2 ? 2u : 1u); i++) {
                uint32_t nn = i ? n.n2 : n.n1, oc = nn / 12u, w = 3u + (uint32_t)n.len * 10u / 15u;
                uint16_t col = ui_mix(RGB(130, 70, 20), RGB(255, 235, 170), oc < 2u ? 0u : oc > 9u ? 16u : (oc - 2u) * 2u);
                if (i) col = ui_mix(col, C_BLACK, 5u);
                cv_rect((int32_t)c * 15, (int32_t)(11u - nn % 12u) * 8, (int32_t)w, 7, col);
            }
        }
    }
    if (stepedit >= 0 && (uint32_t)stepedit / 16u == seqview) frame_col((uint32_t)stepedit % 16u);
    cv_blit(0, 60);
}
static void draw_row(unsigned row, unsigned p)
{
    if (engine_val() == 2u || seqmode) draw_cell(row, p);
    else if (engine_val()) draw_readout(row, p);
    else draw_knob(row, p);
}
static void draw_radar(uint32_t sw)                              /* sw: the sweep's angle, degrees clockwise from the top */
{
    int32_t c = RADAR_R + 2, c4 = c * 4, t, i, k;
    cv_begin(124, 124, C_BLACK);
    ui_disc(c4, c4, RADAR_R * 4, C_PH0);
    for (i = 1; i <= 3; i++) ui_ring(c4, c4, i * 80, 2, C_PH1);  /* range rings */
    cv_line(c, 2, c, 122, C_PH1);                                /* the cross */
    cv_line(2, c, 122, c, C_PH1);
    for (i = 0; i < 360; i += 30) {                              /* bearing ticks */
        int32_t sn = ui_sin(i), cs = ui_cos(i);
        cv_line(c + ((57 * sn) >> 14), c - ((57 * cs) >> 14), c + ((60 * sn) >> 14), c - ((60 * cs) >> 14), C_PH2);
    }
    for (t = 30; t >= 0; t--) {                                  /* the sweep and its fading trail */
        int32_t a = (int32_t)sw - t, lv = 5 - t * 5 / 30;
        uint16_t col = lv >= 5 ? C_PH4 : lv >= 4 ? C_PH3 : lv >= 2 ? C_PH2 : lv >= 1 ? C_PH1 : C_PH0;
        int32_t ex = (RADAR_R * ui_sin(a)) >> 14, ey = (RADAR_R * ui_cos(a)) >> 14;
        int32_t px = (RADAR_R * ui_sin(a - 1)) >> 14, py = (RADAR_R * ui_cos(a - 1)) >> 14;
        cv_line(c, c, c + ex, c - ey, col);
        cv_line(c, c, c + (ex + px) / 2, c - (ey + py) / 2, col);      /* (between two degrees: no gaps at the rim) */
    }
    for (k = 0; k < 128; k++) {                                  /* a blip for each held note: bearing = note name, range = pitch */
        int32_t nn, ang, r, dd, lv;
        if (!(nheld[k >> 5] >> (k & 31) & 1u)) continue;
        nn = k < 36 ? 36 : k > 96 ? 96 : k;
        ang = (k % 12) * 30 + 15;
        r = 8 + (nn - 36) * 48 / 60;
        dd = ((int32_t)sw - ang + 720) % 360;                    /* how long ago the sweep passed it */
        lv = dd < 200 ? 16 - dd * 12 / 200 : 4;
        ui_disc(c4 + ((r * 4 * ui_sin(ang)) >> 14), c4 - ((r * 4 * ui_cos(ang)) >> 14), 11, ui_mix(C_PH4, C_PH0, (uint32_t)lv));
    }
    if (ping) {                                                  /* a ping goes out when a note starts */
        uint32_t lv = 16u - ping * 16u / (RADAR_R + 3u);
        ui_ring(c4, c4, (int32_t)ping * 4, 3, ui_mix(C_PH3, C_PH0, lv));
        ping += 3u;
        if (ping > RADAR_R) ping = 0;
    }
    ui_ring(c4, c4, RADAR_R * 4, 4, RGB(34, 110, 60));           /* the bezel */
    ui_disc(c4, c4, 8, C_PH4);
    cv_blit(RADAR_X, RADAR_Y);
}
static void draw_header(unsigned page, uint32_t lamp)
{
    uint32_t en = engine_val(), n = seqmode ? val[SQ_P + SEQ_LEN] + 1u : npage(), i, vw = seqmode ? seqview : page, pp = seq.run ? seq.pos / 16u : 99u;
    char tt[12] = "STEP ";
    const char *name = seqmode ? "SEQ" : ENGINE_NAMES[en], *title = page_title(page);
    if (seqmode && stepedit >= 0) { put_num(tt + 5, (uint32_t)stepedit + 1u); title = tt; }
    uint16_t c_name = en == 2u ? C_DR : en ? C_PH3 : C_CREAM, c_title = en == 2u ? C_DRL : en ? C_PH2 : C_CREAM_DIM;
    uint16_t c_on = en == 2u ? C_WHITE : en ? C_PH4 : C_ACC, c_off = en == 2u ? C_DRD : en ? C_PH1 : C_LEGEND;
    int32_t x, w;
    cv_begin(240, 40, C_BLACK);
    cv_text(8, 2, &FONT_L, name, c_name);
    w = text_w(&FONT_L, name);
    if (!en) {                                                   /* the red lamp: a note sounds */
        ui_disc((8 + w + 18) * 4, 19 * 4, lamp ? 28 : 24, lamp ? C_RED : RGB(80, 12, 10));
        if (lamp) ui_ring((8 + w + 18) * 4, 19 * 4, 40, 4, RGB(150, 24, 18));
    }
    w = text_w(&FONT_S, title);
    cv_text(236 - w, 2, &FONT_S, title, c_title);
    x = 236 - (int32_t)(n * 10u - 4u);
    for (i = 0; i < n; i++)                                      /* the page marks */
        cv_rect(x + (int32_t)i * 10, 24, 6, 6, i == vw ? c_on : (seqmode && i == pp) ? C_DR : c_off);
    cv_rect(0, 39, 240, 1, c_off);
    cv_blit(0, 0);
}
static void draw_page(unsigned page)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_header(page, gate() != 0);
}
static void draw_preset(void)
{
    char t[48];
    uint32_t n = 0, i = cur;
    if (fm1_ms < msg_until) {
        while (msg[n]) { t[n] = msg[n]; n++; }
        t[n] = 0;
        draw_text_box(8, 44, 224, &FONT_S, t, C_ACC, 0);
        return;
    }
    if (seqmode) {                                               /* SEQ: the pattern, the pad, the page */
        uint32_t o = 0;
        const char *nm = engine_val() == 2u ? drums_names[val[DSEL_P]] : "NOTES";
        t[o++] = 'P'; put_num(t + o, cur_pat() + 1u); while (t[o]) o++;
        t[o++] = ' '; t[o++] = ' ';
        while (*nm) t[o++] = *nm++;
        t[o++] = ' '; t[o++] = ' '; t[o++] = 'B'; t[o++] = 'A'; t[o++] = 'R'; t[o++] = ' ';
        put_num(t + o, (uint32_t)seqview + 1u); while (t[o]) o++;
        t[o++] = '/'; put_num(t + o, val[SQ_P + SEQ_LEN] + 1u);
        draw_text_box(8, 44, 224, &FONT_S, t, C_DRL, 0);
        return;
    }
    if (i < NFACTORY) {
        const char *nm = FACTORY_NAME[i];
        t[n++] = 'F'; if (i + 1u >= 10u) t[n++] = (char)('0' + (i + 1u) / 10u); t[n++] = (char)('0' + (i + 1u) % 10u); t[n++] = ' ';
        while (*nm && n < 28u) t[n++] = *nm++;
    } else {
        uint32_t u = i - NFACTORY + 1u;
        t[n++] = 'U'; t[n++] = (char)('0' + u / 10u); t[n++] = (char)('0' + u % 10u);
        if (!ubank.used[u - 1u]) { const char *e = " EMPTY"; while (*e) t[n++] = *e++; }
    }
    if (modified) t[n++] = '*';
    t[n] = 0;
    draw_text_box(8, 44, 224, &FONT_S, t, engine_val() == 2u ? C_DRL : engine_val() ? C_PH2 : C_CREAM_DIM, 0);
}
static void draw_status(int32_t oct, uint32_t ch, uint32_t src)
{
    char s[32] = "OCT +0  MIDI OMNI";
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    if (ch) { s[13] = 'C'; s[14] = 'H'; s[15] = (char)(ch < 10u ? ' ' : '1'); s[16] = (char)('0' + ch % 10u); }
    if (src) { s[17] = ' '; s[18] = ' '; s[19] = 'U'; s[20] = src == 2u ? 'A' : 'S'; s[21] = src == 2u ? 'R' : 'B'; s[22] = src == 2u ? 'T' : 0; s[23] = 0; }
    draw_text_box(0, 220, 240, &FONT_S, s, C_GREY, 1);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
typedef struct { uint8_t magic, page, oct, ch, preset; uint8_t p[NPARAM]; } settings_t;
static settings_t set = {0xA9, 0, 0, 0, 0, {0}};
static uint32_t idle_ok, midi_src;

static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {   /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u;
        if (st >= 0xF0u) {                          /* real time: MIDI clock and transport drive the drum sequencer when Sync is Ext */
            if (engine_val() != 1u && val[SQ_P + SEQ_SYNC]) {
                if (st == 0xF8u) push(EV_SEQ, SEQ_CLOCK, 0, 0);
                else if (st == 0xFAu) push(EV_SEQ, SEQ_START, 0, 0);
                else if (st == 0xFBu) push(EV_SEQ, SEQ_CONTINUE, 0, 0);
                else if (st == 0xFCu) push(EV_SEQ, SEQ_STOP, 0, 0);
            }
            continue;
        }
        if (set.ch && (st & 15u) != set.ch - 1u) continue;
        midi_src = (uint32_t)src;
        if (ty == 0x90u && d2) play(d1, d2);
        else if (ty == 0x80u || ty == 0x90u) stop(d1);
        else if (ty == 0xC0u) load_preset(d1);
        else if (ty == 0xE0u) push(EV_BEND, 0, 0, (int16_t)((int)d1 + ((int)d2 << 7) - 8192));
        else if (ty == 0xB0u) {
            if (d1 == 1u) push(EV_MOD, d2, 0, 0);
            else if (d1 == 64u) push(EV_PEDAL, d2 >= 64u, 0, 0);
            else if (d1 == 120u || d1 == 123u) { push(EV_PANIC, 0, 0, 0); nheld[0] = nheld[1] = nheld[2] = nheld[3] = 0; }
        }
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

/* the panel's section buttons: the first page of the section and how many pages it has */
typedef struct { uint8_t btn, first, count; } section_t;
static const section_t SECTION_PV[] = {
    {BTN_SEL, 0, 1}, {BTN_EDIT, 1, 2}, {BTN_ENV, 3, 2}, {BTN_LFO, 5, 2}, {BTN_GLO, 7, 1}, {BTN_FX, 8, 1}};
static const section_t SECTION_SI[] = {
    {BTN_SEL, 1, 1}, {BTN_EDIT, 0, 1}, {BTN_ENV, 2, 1}, {BTN_LFO, 0, 1}, {BTN_GLO, 2, 1}, {BTN_FX, 3, 1}};
static const section_t SECTION_DR[] = {
    {BTN_SEL, 0, 1}, {BTN_EDIT, 1, 1}, {BTN_ENV, 0, 1}, {BTN_LFO, 1, 1}, {BTN_GLO, 1, 1}, {BTN_FX, 2, 1}};
#define NSECTION 6u
static const section_t *sections(void) { return engine_val() == 2u ? SECTION_DR : engine_val() ? SECTION_SI : SECTION_PV; }

static void splash(void)                        /* the boot screen: the BEAT CULT sleeper, the name, the two engines */
{
    uint32_t i, a, t0;
    uint16_t ramp[16];
    int32_t w;
    for (a = 0; a < 16u; a++) ramp[a] = ui_mix(C_CREAM, C_BLACK, a);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    cv_begin(LOGO_W, LOGO_H, C_BLACK);
    for (i = 0; i < LOGO_W * LOGO_H; i++) {
        uint32_t v = LOGO[i / 2u];
        v = (i & 1u) ? (v & 15u) : (v >> 4);
        if (v) cv_pset((int32_t)(i % LOGO_W), (int32_t)(i / LOGO_W), ramp[v]);
    }
    cv_blit(60, 20);
    w = text_w(&FONT_L, "BEAT CULT");
    draw_text_box((uint32_t)(120 - w / 2) - 10u, 156, (uint32_t)w + 20u, &FONT_L, "BEAT CULT", C_CREAM, 1);
    draw_text_box(0, 200, 240, &FONT_S, "PLVKS  DUBMARINE  DRUMS", C_CREAM_DIM, 1);
    lcd_sync();
    t0 = fm1_ms;
    #ifdef DINGHY_HOST
    (void)t0;                                   /* (the computer shows it for no time: the test scripts keep their clock) */
#else
    for (i = 0; i < 4000u && fm1_ms - t0 < 1800u; i++) platform_poll();
#endif
}

static void app_main(void)
{
    const section_t *SECTION = SECTION_PV;
    uint32_t gsig = ~0u, hsig = ~0u, last_pos = 255u, play_mod = 0, pad_sig = 0, ui_reset_pads = 1, engine_drawn = 0, t_radar = 0, sw = 0, lamp_drawn = 0, pl_sig = ~0u, self, t_dsp = 0, dsp_drawn = ~0u, prev = 0, k, t_change = 0, dirty = 0, page_drawn = ~0u, status_drawn = ~0u, p;
    uint8_t held[27] = {0};
    uint8_t drawn[4];
    uint32_t knob_t[4] = {0, 0, 0, 0}, home = 0, sf_was = 0, step_used = 0;
    int32_t sfdrawn[4] = {0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};
    uint8_t held_k = 255u;
    polivoks_bank_t *bank = (polivoks_bank_t *)bank_store;
    dubsiren_bank_t *siren = (dubsiren_bank_t *)siren_store;

    if (blob_load(0, &ubank, sizeof ubank)) {
        uint32_t z, u;
        uint8_t *r = (uint8_t *)&ubank;
        for (z = 0; z < sizeof ubank; z++) r[z] = 0;
        if (!blob_load(0, &old_bank, sizeof old_bank))      /* the first presets (36 values, 7.2 ms echo scale): carry them over */
            for (u = 0; u < NUSER; u++) if (old_bank.used[u]) {
                defaults();
                for (z = 0; z < 32u; z++) ubank.p[u][z] = old_bank.p[u][z];
                for (z = 32u; z < PRESET_N; z++) ubank.p[u][z] = val[z];
                for (z = 0; z < 4u; z++) ubank.p[u][FX_P + z] = old_bank.p[u][32u + z];
                ubank.p[u][FX_P] = (uint8_t)(old_bank.p[u][32] * 42u / 59u);   /* the echo time range grew: same time as before */
                ubank.used[u] = 1;
            }
        defaults();
    }
    defaults();
    for (p = SQ_P; p < NPARAM; p++) val[p] = param(p)->def;
    if (blob_load(1, &pbank, sizeof pbank)) { uint8_t *r = (uint8_t *)&pbank; for (k = 0; k < sizeof pbank; k++) r[k] = 0; }
    p = (uint32_t)settings_load(&set, sizeof set);
    if (p) {                                    /* the settings of the build before the sequencer: 5 + 84 bytes; carry them over */
        uint8_t ob[5u + SQ_P];
        if (!settings_load(ob, sizeof ob)) {
            for (k = 0; k < sizeof ob; k++) ((uint8_t *)&set)[k] = ob[k];
            for (k = SQ_P; k < NPARAM; k++) set.p[k] = param(k)->def;
            p = 0;
        }
    }
    if (p || set.magic != 0xA9 || (int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u || set.page >= MAXPAGE || set.preset >= NPRESET) {
        set.magic = 0xA9; set.preset = 0; set.page = 0; set.oct = 0; set.ch = 0;
    } else {
        for (p = 0; p < NPARAM; p++) if (set.p[p] <= param(p)->max) val[p] = set.p[p];
    }
    cur = set.preset;
    self = selftest();
#ifdef DINGHY_HOST
    printf("selftest %u\n", (unsigned)self);
#endif
    polivoks_bank_init(bank);                   /* before the audio interrupt may touch them */
    dubsiren_init(siren);
    drums_init((drums_bank_t *)drums_store);
    seq_init(&seq, (drums_bank_t *)drums_store, bank);
    polivoks_fx_reset();
    for (p = 0; p < NPARAM; p++) apply(p, val[p]);
    engine = (uint8_t)val[ENGINE_P];
    pattern_load(cur_pat());
    ready = 1;
    splash();

    {
        char t[40] = "FLOAT ", *q = t + 6;
        uint32_t d = self > SELFTEST_EXPECT ? self - SELFTEST_EXPECT : SELFTEST_EXPECT - self;
        const char *w = d * 50u <= SELFTEST_EXPECT ? "OK " : "BAD ";
        while (*w) *q++ = *w++;
        if (*(q - 2) != 'K') put_big(q, self); else q[-1] = 0;
        {
            uint32_t n; for (n = 0; t[n] && n < 39u; n++) self_msg[n] = t[n];
            self_msg[n] = 0;
        }
    }
    for (;;) {
        uint32_t keys, down, up, rel, b, out_ch = set.ch ? set.ch - 1u : 0u, st, page = seqmode ? seqpg : set.page;
        if (page >= npage()) { page = 0; if (seqmode) seqpg = 0; else set.page = 0; }
        SECTION = sections();
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        ensure_bank();
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u && seqmode) {         /* SEQ: white keys are steps (a tap on release), black keys pick the pad */
                if (KEY_STEP[k] >= 0) {
                    held_k = (uint8_t)k; step_used = 0;
                    if (stepedit >= 0) { edit_enter((uint32_t)seqview * 16u + (uint32_t)KEY_STEP[k]); step_used = 1; }   /* editing: the next step */
                } else if (KEY_PAD[k] >= 0 && KEY_PAD[k] < (int)DRUMS_NPAD && engine_val() == 2u) {
                    set_param(DSEL_P, (unsigned)KEY_PAD[k]); pat_ver++; ui_reset = 1; stepedit = -1;
                    play(drums_note_of_pad((uint32_t)KEY_PAD[k]), 100); stop(drums_note_of_pad((uint32_t)KEY_PAD[k]));
                }
            } else if (down >> k & 1u) {
                held[k] = engine_val() == 2u ? (k < DRUMS_NPAD ? (uint8_t)drums_note_of_pad(k) : 0u) : (uint8_t)(base + k);   /* DRUMS: the first ten keys are the pads */
                if (held[k]) { play(held[k], 100); midi_send(0x90u | out_ch, held[k], 100); }
            }
            if (up >> k & 1u) {
                if (seqmode && k == held_k) {
                    held_k = 255u;
                    if (!step_used && stepedit < 0) step_tap((uint32_t)seqview * 16u + (uint32_t)KEY_STEP[k]);
                }
                if (held[k]) { stop(held[k]); midi_send(0x80u | out_ch, held[k], 0); }
                held[k] = 0;
            }
        }

        b = fm1_input_edges(&rel);
        home = fm1_in.buttons >> BTN_HOME & 1u;
        if (seqmode) {                             /* SEQ: OCT- OCT+ turn the page of 16 steps */
            if (b & (1u << BTN_OCT_DOWN) && seqview > 0) { seqview--; stepedit = -1; pat_ver++; }
            if (b & (1u << BTN_OCT_UP) && seqview < val[SQ_P + SEQ_LEN]) { seqview++; stepedit = -1; pat_ver++; }
        } else if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (!seqmode && b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            if (fm1_in.buttons >> BTN_PLAY & 1u) {         /* PLAY held: the MIDI channel, OMNI or 1..16 */
                int32_t c = (int32_t)set.ch + (e > 0 ? 1 : -1);
                char t[16] = "MIDI OMNI";
                play_mod = 1;
                set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
                if (set.ch) { t[5] = 'C'; t[6] = 'H'; t[7] = ' '; put_num(t + 8, set.ch); }
                say(t, 1200);
                dirty = 1; t_change = fm1_ms;
            } else if ((int32_t)engine_val() + (e > 0 ? 1 : -1) >= 0 && (int32_t)engine_val() + (e > 0 ? 1 : -1) <= 2) {   /* the engine: one click, one step */
                set_param(ENGINE_P, (unsigned)((int32_t)engine_val() + (e > 0 ? 1 : -1)));
                nheld[0] = nheld[1] = nheld[2] = nheld[3] = 0;
                seqmode = 0; stepedit = -1; held_k = 255u; page = 0; set.page = 0; modified = 1; ui_reset = 1; dirty = 1; t_change = fm1_ms;
                SECTION = sections();
            }
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {        /* the preset: loaded as you turn */
            int32_t c = (int32_t)cur + (e > 0 ? 1 : -1);
            c = c < 0 ? 0 : c >= (int32_t)NPRESET ? (int32_t)NPRESET - 1 : c;
            if ((uint32_t)c != cur) { load_preset((uint32_t)c); dirty = 1; t_change = fm1_ms; }
        }
        if (b & (1u << BTN_SAVE)) { if (seqmode) { pattern_store(cur_pat()); say("PATTERN SAVED", 1200); } else save_preset(); }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            int32_t c = (int32_t)page + (e > 0 ? 1 : -1);
            page = (uint32_t)(c < 0 ? npage() - 1u : c >= (int32_t)npage() ? 0 : c);
            if (seqmode) seqpg = page; else set.page = (uint8_t)page;
            dirty = 1; t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++) {
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                if (seqmode && stepedit >= 0) {                        /* the edited step's four values */
                    int32_t v, mn, mx, ae = e < 0 ? -e : e, step, nv;
                    sf_get(k, &v, &mn, &mx);
                    if (mx - mn <= 8) step = e > 0 ? 1 : -1;
                    else {
                        uint32_t dt = fm1_ms - knob_t[k], m = dt > 160u ? 1u : dt > 90u ? 2u : dt > 45u ? 4u : 8u;
                        if (ae >= 2) m = m < 4u ? 4u : 8u;
                        step = e * (int32_t)m;
                    }
                    knob_t[k] = fm1_ms;
                    nv = v + step;
                    nv = nv < mn ? mn : nv > mx ? mx : nv;
                    if (nv != v) sf_set(k, nv);
                    continue;
                }
                unsigned q = page_param(page, k), mx = param(q)->max;
                int32_t v, ae = e < 0 ? -e : e, step;
                if (mx <= 8u) step = e > 0 ? 1 : -1;                   /* a choice: one detent, one choice */
                else {                                                  /* by speed: slow = 1 a click, spun = up to 8 */
                    uint32_t dt = fm1_ms - knob_t[k], m = dt > 160u ? 1u : dt > 90u ? 2u : dt > 45u ? 4u : 8u;
                    if (ae >= 2) m = m < 4u ? 4u : 8u;
                    step = e * (int32_t)m;
                }
                knob_t[k] = fm1_ms;
                v = (int32_t)val[q] + step;
                v = v < 0 ? 0 : v > (int32_t)mx ? (int32_t)mx : v;
                if ((uint32_t)v != val[q]) {
                    if (q == SQ_P + SEQ_PATTERN) { if (pat_dirty) pattern_store(val[q]); set_param(q, (unsigned)v); pattern_load((uint32_t)v); }
                    else set_param(q, (unsigned)v);
                    if (q >= SQ_P) { if (q != SQ_P + SEQ_PATTERN && q != SQ_P + SEQ_SYNC) { pat_dirty = 1; pat_t = fm1_ms; } }
                    else modified = 1;
                    dirty = 1; t_change = fm1_ms;
                }
            }
        }
        for (k = 0; k < NSECTION; k++)
            if (!seqmode && b & (1u << SECTION[k].btn)) {
                uint32_t f = SECTION[k].first, n = SECTION[k].count;
                page = (page >= f && page < f + n) ? f + (page - f + 1u) % n : f;
                set.page = (uint8_t)page;
                dirty = 1; t_change = fm1_ms;
            }
        if (seqmode && b & (1u << BTN_EDIT)) {     /* EDIT: hold a step and press it for that step's four knobs; again to leave */
            if (stepedit >= 0) { stepedit = -1; pat_ver++; }
            else if (held_k != 255u && KEY_STEP[held_k] >= 0) { edit_enter((uint32_t)seqview * 16u + (uint32_t)KEY_STEP[held_k]); step_used = 1; }
        }
        if (seqmode && home && b & (1u << BTN_ARP)) copy_bar();       /* HOME + ARP: this bar into the next */
        if (b & (1u << BTN_SEQ)) {
            if (seqmode && home) {                                   /* HOME + SEQ: clear the pattern */
                seq_clear(patbuf[pat_live]); stepedit = -1;
                touch_pat(); say("PATTERN CLEARED", 1500);
            } else if (engine_val() != 1u) { seqmode = !seqmode; stepedit = -1; held_k = 255u; seqpg = 0; seqview = 0; page_drawn = ~0u; pat_ver++; }
            else say("SEQ: PLVKS OR DRUMS", 1200);
        }
        if (rel & (1u << BTN_PLAY)) {             /* PLAY (on release: held with ALGORITHM it sets the MIDI channel): start and stop */
            if (!play_mod && engine_val() != 1u) push(EV_SEQ, seq.run ? SEQ_STOP : SEQ_START, 0, 0);
            play_mod = 0;
        }
        if (seqmode && b & (1u << BTN_REC)) {     /* REC: clear the selected pad's row (PLVKS: this bar's notes) */
            if (bank_kind == SEQ_KIND_DRUMS) { for (k = 0; k < SEQ_STEPS; k++) seq_put(patbuf[pat_live], k, val[DSEL_P], 0); say("ROW CLEARED", 1200); }
            else {
                for (k = 0; k < 16u; k++) { seq_note_t r; seq_note_get(patbuf[pat_live], (uint32_t)seqview * 16u + k, &r); r.on = 0; seq_note_put(patbuf[pat_live], (uint32_t)seqview * 16u + k, &r); }
                say("BAR CLEARED", 1200);
            }
            stepedit = -1; touch_pat();
        }
        if (b & (1u << BTN_HOME)) {
            if (seqmode) say("+SEQ CLEAR +ARP COPY +SAVE", 3000);   /* HOME in SEQ: ask first */
            else {                                                    /* HOME: every value back to the starting sound */
                defaults();
                for (p = 0; p < NPARAM; p++) push(EV_PARAM, (uint8_t)p, val[p], 0);
                for (k = 0; k < 4u; k++) drawn[k] = 0xFF;
                modified = 1; dirty = 1; t_change = fm1_ms;
            }
        }
        if (aud_n[0] != 255u && fm1_ms - aud_t > 180u) {               /* an audition ends */
            stop(aud_n[0]); if (aud_n[1] != 255u) stop(aud_n[1]);
            aud_n[0] = aud_n[1] = 255u;
        }
        if ((seqmode && stepedit >= 0) != sf_was) { sf_was = seqmode && stepedit >= 0; ui_reset = 1; }

        midi_in();
        if (ui_reset) { ui_reset = 0; for (k = 0; k < 4u; k++) { drawn[k] = 0xFF; sfdrawn[k] = 0x7FFFFFFF; } }

        for (k = 0; k < 27u; k++) {
            uint32_t on = keys >> k & 1u;
            if (seqmode) {                                   /* SEQ: the selected pad's steps; the playhead flips its key; black keys: the pad */
                if (KEY_STEP[k] >= 0) {
                    uint32_t step = (uint32_t)seqview * 16u + (uint32_t)KEY_STEP[k];
                    if (bank_kind == SEQ_KIND_DRUMS) on = seq_get(patbuf[pat_live], step, val[DSEL_P]) != 0;
                    else { seq_note_t r; seq_note_get(patbuf[pat_live], step, &r); on = r.on; }
                    if (seq.run && seq.pos == step) on = !on;
                    if (stepedit == (int32_t)step) on = 1;
                } else on = engine_val() == 2u && KEY_PAD[k] == (int)val[DSEL_P];
            }
            fm1_led_key(KEY_LED(k), on);
        }
        for (k = 0; k < NSECTION; k++)
            fm1_led_key(SECTION[k].btn, !seqmode && page >= SECTION[k].first && page < SECTION[k].first + SECTION[k].count);
        fm1_led_key(BTN_OCT_DOWN, !seqmode && oct < 0);
        fm1_led_key(BTN_OCT_UP, !seqmode && oct > 0);
        fm1_led_key(BTN_SEQ, seqmode);
        fm1_led_key(BTN_PLAY, seq.run);

        idle_ok = 1;
        if (engine_val() != engine_drawn) { engine_drawn = engine_val(); page_drawn = ~0u; }
        if (page != page_drawn) { page_drawn = page; draw_page(page); lamp_drawn = gate() != 0; t_radar = 0; ui_reset_pads = 1; gsig = hsig = ~0u; for (k = 0; k < 4u; k++) drawn[k] = 0xFF; status_drawn = ~0u; dsp_drawn = ~0u; pl_sig = ~0u;
 }
        if (!engine_val() && (gate() != 0) != lamp_drawn) { lamp_drawn = gate() != 0; draw_header(page, lamp_drawn); }
        if (engine_val() == 2u && seq.run && seq.pos != last_pos) {   /* a step plays: its pads flash */
            last_pos = seq.pos;
            if (last_pos != SEQ_NONE) for (k = 0; k < DRUMS_NPAD; k++) if (seq_get(patbuf[pat_live], last_pos, k)) padhit[k] = 8;
        } else if (!seq.run) last_pos = 255u;
        if (seqmode) {                      /* the step grid and its page marks */
            uint32_t gs = pat_ver * 1000003u ^ (uint32_t)(stepedit + 1) * 65537u ^ bank_kind * 977u ^ (uint32_t)seqview * 7919u ^ val[DSEL_P] * 104729u ^ (seq.run ? ((uint32_t)seq.pos + 1u) * 31u : 0u);
            uint32_t hs = 1u | (uint32_t)seqview << 1 | val[SQ_P + SEQ_LEN] << 4 | (seq.run ? ((uint32_t)seq.pos / 16u + 1u) << 8 : 0u) | (uint32_t)(stepedit + 1) << 16;
            if (page == page_drawn && hs != hsig) { hsig = hs; draw_header(page, 0); }
            if (page == page_drawn && gs != gsig) { gsig = gs; if (engine_val() == 2u) draw_stepgrid(); else draw_notegrid(); }
        } else if (engine_val() == 2u) {                           /* the pad grid: a hit fades over about 0.3 s */
            uint32_t sig = val[DSEL_P], any = 0;
            for (k = 0; k < DRUMS_NPAD; k++) { sig = sig * 9u + padhit[k]; any |= padhit[k]; }
            if (page == page_drawn && (sig != pad_sig || ui_reset_pads)) {
                pad_sig = sig; ui_reset_pads = 0; draw_pads();
            }
            if (any && fm1_ms - t_radar >= 40u) { t_radar = fm1_ms; for (k = 0; k < DRUMS_NPAD; k++) if (padhit[k]) padhit[k]--; }
        }
        if (engine_val() == 1u && fm1_ms - t_radar >= 45u) {            /* the radar: the sweep turns with the LFO rate */
            t_radar = fm1_ms;
            sw = (sw + 2u + (uint32_t)val[SIREN_P + SIREN_RATE] * 10u / 127u) % 360u;
            draw_radar(sw);
        }
        if ((st = cur | modified << 8 | (fm1_ms < msg_until ? 1u << 9 : 0u) | engine_val() << 10 | (uint32_t)seqmode << 12 | (uint32_t)seqview << 13 | val[DSEL_P] << 15 | cur_pat() << 19 | (msg_n & 15u) << 24) != pl_sig) { pl_sig = st; draw_preset(); }
        if (seqmode && stepedit >= 0) {
            for (k = 0; k < 4u; k++) { int32_t v, mn, mx; sf_get(k, &v, &mn, &mx); if (v != sfdrawn[k]) { sfdrawn[k] = v; sf_draw(k, v); } }
        } else
        for (k = 0; k < 4u; k++)
            if (drawn[k] != val[page_param(page, k)] || ((uint32_t)drawn[k] == 0xFFu)) { drawn[k] = val[page_param(page, k)]; draw_row(k, page_param(page, k)); }
        if ((st = (uint32_t)(oct + 2) | (uint32_t)set.ch << 4 | midi_src << 12) != status_drawn) { status_drawn = st; draw_status(oct, set.ch, midi_src); }
        if (fm1_ms - t_dsp > 1000u) {            /* the DSP load: the longest block against its 5.8 ms */
            uint32_t us = dsp_peak, pc = us * 100u / 5805u;
            char d[64];
            uint32_t n = 0, m;
            dsp_peak = 0; t_dsp = fm1_ms;
            for (m = 0; self_msg[m]; m++) d[n++] = self_msg[m];
            d[n++] = ' '; d[n++] = ' '; d[n++] = 'D'; d[n++] = 'S'; d[n++] = 'P'; d[n++] = ' ';
            put_num(d + n, pc > 999u ? 999u : pc);
            while (d[n]) n++;
            d[n++] = '%'; d[n] = 0;
            if (pc != dsp_drawn) { dsp_drawn = pc; draw_text_box(0, 202, 240, &FONT_S, d, pc > 70u ? C_ACC : C_GREY, 1); }
        }
        idle_ok = 0;

        if (pat_dirty && !seq.run && fm1_ms - pat_t > 2000u) pattern_store(cur_pat());   /* (an erase stalls the sound: not while it plays) */
        if (dirty && fm1_ms - t_change > 2000u) {   /* 2 s after the change (an erase stalls the sound; the echo may ring, a short click is possible) */
            for (p = 0; p < NPARAM; p++) set.p[p] = val[p];
            set.preset = (uint8_t)cur;
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
