/* SPDX-License-Identifier: GPL-3.0-only */
/* Polivoks for the M-VAVE FM-1: a duophonic Formanta Polivoks-style voice with a BBD echo,
 * on the Dinghy platform (platform.c: updates, USB rescue, USB-MIDI, keys, knobs, screen, flash).
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
 *   ALGORITHM  the MIDI channel: OMNI or 1..16 (kept in flash)
 *   MIDI       program change selects a preset
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes, pitch bend, mod wheel (CC1), sustain (CC64), all-notes-off (CC123)
 *
 *   PLAY       the engine: Polivoks or dub siren (lit: siren). The siren (dubsiren.c, after beatcult/dubmarine) plays its
 *              pitch from the keys, with a glide for smooth sweeps; its pages are pitch and LFO, filter, play, echo.
 *
 * The voices (polivoks.c, dubsiren.c) and the echo (polivoks_fx.c) are float DSP: the build asks for the FM-1's float
 * hardware (-mfprev1, tools/build.py). tests/host.c runs this file on a computer. */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#include "platform.c"
#ifdef DINGHY_HOST
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return 0; }   /* (no counter on the computer: the meter reads 0) */
#endif
#include "polivoks.c"
#include "polivoks_fx.c"
#include "dubsiren.c"
#include "sirenfmt.h"
#include "presets.h"

#define NVOICEPARAM POLIVOKS_NPARAMS                          /* 0..31 Polivoks */
#define SIREN_P NVOICEPARAM                                    /* 32..43 siren */
#define FX_P (SIREN_P + SIREN_NPARAMS)                         /* 44..47 echo (both engines) */
#define ENGINE_P (FX_P + POLIVOKS_FX_PARAMS)                   /* 48: 0 Polivoks, 1 siren */
#define NPARAM (ENGINE_P + 1)
#define MAXPAGE 9

/* ---- the sound (audio ISR): events from the main loop, one writer each side */
enum { EV_NOTE, EV_PEDAL, EV_BEND, EV_MOD, EV_PARAM, EV_PANIC };
typedef struct { uint8_t type, a, b, pad; int16_t value; } event_t;
enum { RING = 128 };
static event_t ring[RING];
static volatile uint8_t ev_head, ev_tail, ready;
static uint32_t bank_store[POLIVOKS_BANK_BYTES / 4], siren_store[DUBSIREN_BANK_BYTES / 4];
static volatile uint8_t engine;                 /* the engine that sounds (the ISR's copy of ENGINE_P) */
static int32_t mono[POLIVOKS_FRAMES], left[POLIVOKS_FRAMES], right[POLIVOKS_FRAMES];
static volatile uint32_t dsp_peak;               /* the longest audio block since the main loop last looked, in us */
static uint8_t val[NPARAM];                     /* the values on the panel */
static uint32_t engine_val(void) { return val[NPARAM - 1u]; }

static void push(uint8_t type, uint8_t a, uint8_t b, int16_t value)
{
    uint8_t next = (uint8_t)((ev_head + 1u) % RING);
    if (next == ev_tail) return;                /* full: drop, never wait */
    ring[ev_head].type = type; ring[ev_head].a = a; ring[ev_head].b = b; ring[ev_head].value = value;
    ev_head = next;
}
static void apply(unsigned p, unsigned v)       /* ISR side */
{
    if (p < NVOICEPARAM) polivoks_bank_set((polivoks_bank_t *)bank_store, p, v);
    else if (p < FX_P) dubsiren_set((dubsiren_bank_t *)siren_store, p - SIREN_P, v);
    else if (p < ENGINE_P) polivoks_fx_set(p - FX_P, v);
    else if (v != engine) {                      /* the other engine takes over: silence both */
        polivoks_bank_panic((polivoks_bank_t *)bank_store);
        dubsiren_panic((dubsiren_bank_t *)siren_store);
        engine = (uint8_t)v;
    }
}

static void app_audio(int32_t *out, uint32_t n)
{
    polivoks_bank_t *bank = (polivoks_bank_t *)bank_store;
    dubsiren_bank_t *siren = (dubsiren_bank_t *)siren_store;
    uint32_t i, blk, t0 = fm1_ticks(), dt;
    if (!ready) return;
    while (ev_tail != ev_head) {
        const event_t *e = &ring[ev_tail];
        switch (e->type) {
        case EV_NOTE: if (engine) dubsiren_note(siren, e->a, e->b); else polivoks_bank_note(bank, e->a, e->b, 0); break;
        case EV_PEDAL: if (!engine) polivoks_bank_pedal(bank, e->a); break;
        case EV_BEND: if (engine) dubsiren_bend(siren, (int32_t)e->value * 16); else polivoks_bank_bend(bank, (int32_t)e->value * 16); break;   /* +-2 semitones */
        case EV_MOD: if (engine) dubsiren_mod(siren, e->a); else polivoks_bank_mod(bank, e->a); break;
        case EV_PARAM: apply(e->a, e->b); break;
        default: polivoks_bank_panic(bank); dubsiren_panic(siren); break;
        }
        ev_tail = (uint8_t)((ev_tail + 1u) % RING);
    }
    for (blk = 0; blk + POLIVOKS_FRAMES <= n; blk += POLIVOKS_FRAMES) {
        for (i = 0; i < POLIVOKS_FRAMES; i++) mono[i] = 0;
        if (engine) dubsiren_render(siren, mono); else polivoks_bank_render(bank, mono);
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
static const char *const ENGINE_NAMES[] = {"Polivoks", "Siren"};
static const fm1_param_t ENGP = {"Engine", 1, 0, ENGINE_NAMES};
static const fm1_param_t *param(unsigned p)
{
    return p < NVOICEPARAM ? polivoks_param((int)p) : p < FX_P ? dubsiren_param((int)(p - SIREN_P))
         : p < ENGINE_P ? &FXP[p - FX_P] : &ENGP;
}
/* pages: the first parameter of each (four to a page), per engine */
static const uint8_t PV_PAGE[9] = {0, 4, 8, 12, 16, 20, 24, 28, FX_P};
static const char *const PV_TITLE[9] = {"FILTER", "OSCILLATOR 1", "OSCILLATOR 2", "AMP ENVELOPE", "FILTER ENV",
                                        "LFO", "MOD AND DRIVE", "PLAY", "BBD ECHO"};
static const uint8_t SI_PAGE[4] = {SIREN_P, SIREN_P + 4, SIREN_P + 8, FX_P};
static const char *const SI_TITLE[4] = {"SIREN", "SIREN FILTER", "SIREN PLAY", "BBD ECHO"};
static uint32_t npage(void) { return engine_val() ? 4u : 9u; }
static uint32_t page_base(uint32_t page) { return engine_val() ? SI_PAGE[page] : PV_PAGE[page]; }
static const char *page_title(uint32_t page) { return engine_val() ? SI_TITLE[page] : PV_TITLE[page]; }

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
}

/* ---- presets: NFACTORY in the code, NUSER in flash (blob 0, one sector: saved on SAVE) */
#define NUSER 16u
#define NPRESET (NFACTORY + NUSER)
static struct { uint8_t used[NUSER]; uint8_t p[NUSER][NPARAM]; } ubank;
static struct { uint8_t used[NUSER]; uint8_t p[NUSER][36]; } old_bank;   /* the layout of the first preset build */
static volatile uint32_t cur, modified, ui_reset;
static char msg[16];
static uint32_t msg_until;

static void load_preset(uint32_t i)             /* main loop, or MIDI program change */
{
    unsigned p;
    if (i >= NPRESET) return;
    if (i < NFACTORY) for (p = 0; p < NPARAM; p++) val[p] = FACTORY[i][p];
    else if (ubank.used[i - NFACTORY]) for (p = 0; p < NPARAM; p++) val[p] = ubank.p[i - NFACTORY][p];
    else defaults();
    for (p = 0; p < NPARAM; p++) {
        if (val[p] > param(p)->max) val[p] = param(p)->max;
        push(EV_PARAM, (uint8_t)p, val[p], 0);
    }
    cur = i; modified = 0; ui_reset = 1; msg_until = 0;
}
static void say(const char *s, uint32_t ms)
{
    uint32_t n = 0;
    while (s[n] && n < 15u) { msg[n] = s[n]; n++; }
    msg[n] = 0;
    msg_until = fm1_ms + ms;
}
static void save_preset(void)                   /* SAVE: into the selected user slot, or the next free one */
{
    uint32_t slot = NUSER, k;
    char t[16] = "SAVED U";
    if (cur >= NFACTORY) slot = cur - NFACTORY;
    else for (k = 0; k < NUSER; k++) if (!ubank.used[k]) { slot = k; break; }
    if (slot >= NUSER) { say("BANK FULL", 1500); return; }
    for (k = 0; k < NPARAM; k++) ubank.p[slot][k] = val[k];
    ubank.used[slot] = 1;
    if (blob_save(0, &ubank, sizeof ubank)) { say("SAVE FAILED", 2000); return; }
    cur = NFACTORY + slot; modified = 0;
    t[7] = (char)('0' + (slot + 1u) / 10u); t[8] = (char)('0' + (slot + 1u) % 10u); t[9] = 0;
    say(t, 1500);
}

/* ---- notes (main loop) */
static uint32_t last_note = 255;
static void play(uint32_t note, uint32_t vel)
{
    if (note > 127u || !vel) return;
    push(EV_NOTE, (uint8_t)note, (uint8_t)vel, 0);
    last_note = note;
}
static void stop(uint32_t note) { if (note <= 127u) push(EV_NOTE, (uint8_t)note, 0, 0); }

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
static void draw_row(unsigned row, unsigned p)
{
    const fm1_param_t *d = param(p);
    char s[24];
    int32_t y = 66 + (int32_t)row * 34;
    format_param(p, s);
    draw_text_box(8, y, 100, &FONT_S, d->name, C_GREY, 0);
    draw_text_box(112, y, 120, &FONT_S, s, C_WHITE, 2);
    lcd_fill(8, y + 20, 224, 6, C_TRACK);
    lcd_fill(8, y + 20, (uint32_t)val[p] * 224u / (d->max ? d->max : 1u), 6, C_ACC);
}
static void draw_page(unsigned page)
{
    char s[8] = "1/9";
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 8, 240, &FONT_L, page_title(page), C_ACC, 1);
    s[0] = (char)('1' + page);
    s[2] = (char)('0' + npage());
    draw_text_box(190, 44, 50, &FONT_S, s, C_GREY, 2);
}
static void draw_preset(void)
{
    char t[32];
    uint32_t n = 0, i = cur;
    if (fm1_ms < msg_until) {
        while (msg[n]) { t[n] = msg[n]; n++; }
        t[n] = 0;
        draw_text_box(0, 44, 190, &FONT_S, t, C_ACC, 0);
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
    draw_text_box(0, 44, 190, &FONT_S, t, C_GREY, 0);
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
        if (st >= 0xF0u) continue;                  /* clock and transport: not used */
        if (set.ch && (st & 15u) != set.ch - 1u) continue;
        midi_src = (uint32_t)src;
        if (ty == 0x90u && d2) play(d1, d2);
        else if (ty == 0x80u || ty == 0x90u) stop(d1);
        else if (ty == 0xC0u) load_preset(d1);
        else if (ty == 0xE0u) push(EV_BEND, 0, 0, (int16_t)((int)d1 + ((int)d2 << 7) - 8192));
        else if (ty == 0xB0u) {
            if (d1 == 1u) push(EV_MOD, d2, 0, 0);
            else if (d1 == 64u) push(EV_PEDAL, d2 >= 64u, 0, 0);
            else if (d1 == 120u || d1 == 123u) push(EV_PANIC, 0, 0, 0);
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
#define NSECTION 6u

static void app_main(void)
{
    const section_t *SECTION = SECTION_PV;
    uint32_t engine_drawn = 0, pl_sig = ~0u, self, t_dsp = 0, dsp_drawn = ~0u, prev = 0, k, t_change = 0, dirty = 0, page_drawn = ~0u, status_drawn = ~0u, p;
    uint8_t held[27] = {0};
    uint8_t drawn[4];
    uint32_t knob_t[4] = {0, 0, 0, 0};
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
                for (z = 32u; z < NPARAM; z++) ubank.p[u][z] = val[z];
                for (z = 0; z < 4u; z++) ubank.p[u][FX_P + z] = old_bank.p[u][32u + z];
                ubank.p[u][FX_P] = (uint8_t)(old_bank.p[u][32] * 42u / 59u);   /* the echo time range grew: same time as before */
                ubank.used[u] = 1;
            }
        defaults();
    }
    defaults();
    if (settings_load(&set, sizeof set) || set.magic != 0xA9 || (int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u || set.page >= MAXPAGE || set.preset >= NPRESET) {
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
    polivoks_fx_reset();
    for (p = 0; p < NPARAM; p++) apply(p, val[p]);
    engine = (uint8_t)val[ENGINE_P];
    ready = 1;

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
        uint32_t keys, down, up, rel, b, out_ch = set.ch ? set.ch - 1u : 0u, st, page = set.page;
        if (page >= npage()) page = 0, set.page = 0;
        SECTION = engine_val() ? SECTION_SI : SECTION_PV;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); play(held[k], 100); midi_send(0x90u | out_ch, held[k], 100); }
            if (up >> k & 1u) { stop(held[k]); midi_send(0x80u | out_ch, held[k], 0); }
        }

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {      /* the MIDI channel: OMNI or 1..16 */
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {        /* the preset: loaded as you turn */
            int32_t c = (int32_t)cur + (e > 0 ? 1 : -1);
            c = c < 0 ? 0 : c >= (int32_t)NPRESET ? (int32_t)NPRESET - 1 : c;
            if ((uint32_t)c != cur) { load_preset((uint32_t)c); dirty = 1; t_change = fm1_ms; }
        }
        if (b & (1u << BTN_SAVE)) save_preset();
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            int32_t c = (int32_t)page + (e > 0 ? 1 : -1);
            page = (uint32_t)(c < 0 ? npage() - 1u : c >= (int32_t)npage() ? 0 : c);
            set.page = (uint8_t)page;
            dirty = 1; t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++) {
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                unsigned q = page_base(page) + k, mx = param(q)->max;
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
                if ((uint32_t)v != val[q]) { set_param(q, (unsigned)v); modified = 1; dirty = 1; t_change = fm1_ms; }
            }
        }
        for (k = 0; k < NSECTION; k++)
            if (b & (1u << SECTION[k].btn)) {
                uint32_t f = SECTION[k].first, n = SECTION[k].count;
                page = (page >= f && page < f + n) ? f + (page - f + 1u) % n : f;
                set.page = (uint8_t)page;
                dirty = 1; t_change = fm1_ms;
            }
        if (b & (1u << BTN_PLAY)) {               /* PLAY: the other engine */
            set_param(ENGINE_P, engine_val() ? 0u : 1u);
            page = 0; set.page = 0; modified = 1; ui_reset = 1; dirty = 1; t_change = fm1_ms;
            SECTION = engine_val() ? SECTION_SI : SECTION_PV;
        }
        if (b & (1u << BTN_HOME)) {               /* HOME: every value back to the starting sound */
            defaults();
            for (p = 0; p < NPARAM; p++) push(EV_PARAM, (uint8_t)p, val[p], 0);
            for (k = 0; k < 4u; k++) drawn[k] = 0xFF;
            modified = 1; dirty = 1; t_change = fm1_ms;
        }

        midi_in();
        if (ui_reset) { ui_reset = 0; for (k = 0; k < 4u; k++) drawn[k] = 0xFF; }

        for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), keys >> k & 1u);
        for (k = 0; k < NSECTION; k++)
            fm1_led_key(SECTION[k].btn, page >= SECTION[k].first && page < SECTION[k].first + SECTION[k].count);
        fm1_led_key(BTN_PLAY, engine_val());
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);

        idle_ok = 1;
        if (engine_val() != engine_drawn) { engine_drawn = engine_val(); page_drawn = ~0u; }
        if (page != page_drawn) { page_drawn = page; draw_page(page); for (k = 0; k < 4u; k++) drawn[k] = 0xFF; status_drawn = ~0u; dsp_drawn = ~0u; pl_sig = ~0u;
 }
        if ((st = cur | modified << 8 | (fm1_ms < msg_until ? 1u << 9 : 0u) | engine_val() << 10) != pl_sig) { pl_sig = st; draw_preset(); }
        for (k = 0; k < 4u; k++)
            if (drawn[k] != val[page_base(page) + k] || ((uint32_t)drawn[k] == 0xFFu)) { drawn[k] = val[page_base(page) + k]; draw_row(k, page_base(page) + k); }
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
            if (pc != dsp_drawn) { dsp_drawn = pc; draw_text_box(0, 198, 240, &FONT_S, d, pc > 70u ? C_ACC : C_GREY, 1); }
        }
        idle_ok = 0;

        if (dirty && fm1_ms - t_change > 2000u) {   /* 2 s after the change (an erase stalls the sound; the echo may ring, a short click is possible) */
            for (p = 0; p < NPARAM; p++) set.p[p] = val[p];
            set.preset = (uint8_t)cur;
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
