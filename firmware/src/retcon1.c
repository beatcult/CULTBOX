/* SPDX-License-Identifier: GPL-3.0-only */
/* RETCON-1 for the M-VAVE FM-1: a tape-drift synthesizer (four voices, two PolyBLEP oscillators each, a state-variable
 * filter, wow and flutter, tape saturation and hiss, a tape/BBD delay and a 16-step sequencer) on the Dinghy platform
 * (zvenson) as CULTBOX carries it (platform.c: updates, USB rescue, USB-MIDI, keys, knobs, screen, flash).
 *
 *   keys       play the voices (27 keys, chromatic from F3); four notes at once, the oldest is stolen
 *   SEL        page 1 OSC     wave A, wave B, detune, osc mix
 *   EDIT       page 2 FILT    cutoff, resonance, envelope amount, mode (low-pass, band-pass, high-pass)
 *   LFO        page 3 TAPE    wow, flutter, saturation, hiss
 *   FX         page 4 DLY     time, feedback, damping, mix
 *   ENV        page 5 ENV     attack, decay, sustain, release
 *   GLO        page 6 SEQ     gate length, tempo, mode (fwd, rev, ping-pong, random), swing
 *   SELECT     the next page
 *   KNOB 1-4   the four values of the page (turn fast to go fast); the wave knobs cross four shapes: triangle/saw,
 *              pulse (width), folded sine (fold), reed (brightness)
 *   OCT- OCT+  the octave (kept in flash)
 *   SEQ        step edit on / off. Keys 1-16 switch the steps (the key is the step), keys 17-27 set the selected step's
 *              pitch and move on, PRESETS transposes it, ALGORITHM moves the selection; the key LEDs show the steps
 *   PLAY       start / stop the sequencer (MIDI Start / Stop do the same)
 *   HOME       a tap: all notes off; held 1.5 s: every value back to the starting sound
 *   SAVE       store the settings and the pattern now (they also save themselves 2 s after a change, once all is quiet)
 *   MIDI in    notes, all-notes-off (CC123); CC74 cutoff, CC71 resonance, CC73 attack, CC72 release, CC91 delay mix
 *   MASTER     the volume (platform.c)
 *
 * The engine (dsp/, engine/) is float DSP and runs in the audio interrupt only: the main loop has no float unit, so the
 * panel hands it integers through an event ring, as CULTBOX does. tests/host.c runs this file on a computer. */
#define STORE_MAGIC 0x52544331u                 /* "RTC1": RETCON-1's own settings, never another firmware's */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#define CV_MAX (240u * 96u)                    /* the canvas: the 16 steps are 240 x 92 */
#include "platform.c"
#ifdef CULTBOX_HOST
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return 0; }   /* (no counter on the computer: the meter reads 0) */
#endif
#include "engine/retcon_synth.h"

/* ---- the sound (audio interrupt): events from the main loop, one writer each side */
enum { EV_NOTE, EV_PARAM, EV_PANIC, EV_RUN, EV_STEP, EV_STEP_NOTE };
typedef struct { uint8_t type, a, b, pad; } event_t;
enum { RING = 128 };
static event_t ring[RING];
static volatile uint8_t ev_head, ev_tail, ready;
static rc_synth_t S;                            /* .bss */
static int16_t rc_line[RC_DELAY_SAMPLES] RC_POOL;   /* the delay's tape: the pool, zeroed at boot */
static volatile uint32_t loud_ms;               /* when something last sounded: flash writes wait for quiet (an erase drops the sound) */
static volatile uint32_t dsp_peak;              /* the longest audio block since the main loop last looked, in us */
static volatile uint8_t seq_run_pub, seq_pos_pub;   /* the sequencer, as the panel shows it */

static void push(uint8_t type, uint8_t a, uint8_t b)
{
    uint8_t next = (uint8_t)((ev_head + 1u) % RING);
    if (next == ev_tail) return;                /* full: drop, never wait */
    ring[ev_head].type = type; ring[ev_head].a = a; ring[ev_head].b = b;
    ev_head = next;
}

static void app_audio(int32_t *out, uint32_t n)
{
    static float fb[256];
    uint32_t t0 = fm1_ticks(), done = 0, i, pk = 0, dt;
    if (!ready) return;
    if (!S.ready) rc_synth_init(&S, rc_line);
    while (ev_tail != ev_head) {
        const event_t *e = &ring[ev_tail];
        switch (e->type) {
        case EV_NOTE: rc_synth_note_on(&S, e->a, e->b); break;                     /* b = velocity, 0 = off */
        case EV_PARAM: rc_synth_param127(&S, e->a, e->b); break;
        case EV_RUN: rc_synth_seq_run(&S, e->a); break;
        case EV_STEP:
            if (e->a < RC_SEQ_STEPS) { if (e->b) S.seq.on |= (uint16_t)(1u << e->a); else S.seq.on &= (uint16_t)~(1u << e->a); }
            break;
        case EV_STEP_NOTE: if (e->a < RC_SEQ_STEPS) S.seq.note[e->a] = e->b; break;
        default: rc_synth_all_off(&S); break;                                      /* EV_PANIC */
        }
        ev_tail = (uint8_t)((ev_tail + 1u) % RING);
    }
    while (done < n) {
        uint32_t chunk = n - done > 256u ? 256u : n - done;
        rc_synth_render(&S, fb, chunk);
        for (i = 0; i < chunk; i++) {
            float x = fb[i];
            int32_t v;
            x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
            v = (int32_t)(x * 32767.0f);
            out[2u * (done + i)] += v;
            out[2u * (done + i) + 1u] += v;
            if (v < 0) v = -v;
            if ((uint32_t)v > pk) pk = (uint32_t)v;
        }
        done += chunk;
    }
    if (pk > 40u) loud_ms = fm1_ms;             /* (about -58 dB: something is sounding) */
    seq_run_pub = S.seq.run;
    seq_pos_pub = S.seq.pos;
    dt = (uint32_t)(fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (dt > dsp_peak) dsp_peak = dt;
}

/* ---- the panel */
enum { NPAGE = 6, KEY0 = 53 };                  /* key 0 = F3 (MIDI 53) */
typedef struct { uint8_t magic, ver, page, oct; uint8_t p[RC_P_COUNT]; uint16_t on; uint8_t note[RC_SEQ_STEPS]; } settings_t;
static settings_t set;
static uint8_t val[RC_P_COUNT];                 /* the values on the panel, 0..127 */
static uint8_t sel_step, seqmode, pre_page;
static uint32_t idle_ok, dirty, t_change, knob_t[4], held_note[27], prev_keys;

static void set_val(unsigned id, unsigned v)
{
    if (id >= RC_P_COUNT) return;
    if (v > 127u) v = 127u;
    val[id] = (uint8_t)v;
    push(EV_PARAM, (uint8_t)id, (uint8_t)v);
    dirty = 1;
    t_change = fm1_ms;
}
static void push_pattern(void)
{
    uint32_t s;
    for (s = 0; s < RC_SEQ_STEPS; s++) {
        push(EV_STEP, (uint8_t)s, (uint8_t)(set.on >> s & 1u));
        push(EV_STEP_NOTE, (uint8_t)s, set.note[s]);
    }
}
static void defaults_all(void)
{
    uint32_t i;
    for (i = 0; i < RC_P_COUNT; i++) set_val(i, RC_DEFAULT[i]);
}
static void defaults_pattern(void)              /* the pattern the engine starts with (sequencer.h) */
{
    static const uint8_t NOTES[RC_SEQ_STEPS] = {48, 48, 55, 48, 51, 48, 58, 55, 48, 48, 55, 48, 53, 51, 58, 55};
    uint32_t i;
    for (i = 0; i < RC_SEQ_STEPS; i++) set.note[i] = NOTES[i];
    set.on = 0x1111u | 0x0404u;
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
static void midi_in(void)
{
    uint8_t st, d1, d2;
    static const uint8_t CC[5][2] = {{74, RC_P_CUTOFF}, {71, RC_P_RES}, {73, RC_P_ATTACK}, {72, RC_P_RELEASE}, {91, RC_P_DLY_MIX}};
    while (midi_get(&st, &d1, &d2) != 0) {      /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u, i;
        if (st >= 0xF0u) {
            if (st == 0xFAu || st == 0xFBu) push(EV_RUN, 1, 0);
            else if (st == 0xFCu) push(EV_RUN, 0, 0);
            continue;
        }
        if (ty == 0x90u && d2) push(EV_NOTE, d1, d2);
        else if (ty == 0x80u || ty == 0x90u) push(EV_NOTE, d1, 0);
        else if (ty == 0xB0u) {
            if (d1 == 120u || d1 == 123u) push(EV_PANIC, 0, 0);
            else for (i = 0; i < 5u; i++) if (CC[i][0] == d1) set_val(CC[i][1], d2);
        }
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

/* ---- values as text (integers only) */
static const uint16_t EXP2_Q12[16] = {4096, 4277, 4471, 4675, 4888, 5111, 5344, 5587, 5842, 6108, 6386, 6676, 6980, 7298, 7630, 7977};   /* 2^(i/16) * 4096 */
static uint32_t uexp(uint32_t v, uint32_t min, uint32_t lg_q8)     /* min * 2^(v / 127 * log2(ratio)): lg_q8 = log2(ratio) * 256 */
{
    uint32_t o = (v * lg_q8 + 63u) / 127u;
    return ((min * EXP2_Q12[(o & 255u) >> 4]) << (o >> 8)) >> 12;
}
static uint32_t r_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void r_cpy(char *d, const char *s, uint32_t max)
{
    uint32_t i = 0;
    while (s[i] && i + 1u < max) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void r_fix(char *b, int32_t v, int decimals)             /* r_fix(b, -63, 1) -> "-6.3" */
{
    char t[14];
    int n = 0, neg = v < 0, i;
    uint32_t u = (uint32_t)(neg ? -v : v);
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
        if (n == decimals) t[n++] = '.';
    } while (u || n <= decimals);
    if (neg) t[n++] = '-';
    for (i = 0; i < n; i++) b[i] = t[n - 1 - i];
    b[n] = 0;
}
static void r_int(char *b, int32_t v) { r_fix(b, v, 0); }
static void scat(char *d, const char *s) { r_cpy(d + r_len(d), s, 40u - r_len(d)); }
static void put_pct(char *b, uint32_t p) { r_int(b, (int32_t)p); scat(b, " %"); }
static void put_ms(char *b, uint32_t ms)
{
    if (ms < 1000u) { r_int(b, (int32_t)ms); scat(b, " MS"); }
    else if (ms < 10000u) { r_fix(b, (int32_t)(ms / 100u), 1); scat(b, " S"); }
    else { r_int(b, (int32_t)(ms / 1000u)); scat(b, " S"); }
}
static void put_hz(char *b, uint32_t hz)
{
    if (hz < 1000u) { r_int(b, (int32_t)hz); scat(b, " HZ"); }
    else { r_fix(b, (int32_t)(hz / 100u), 1); scat(b, " KHZ"); }
}
static void put_signed(char *b, int32_t v, const char *unit)
{
    char t[16];
    r_int(t, v);
    b[0] = 0;
    if (v > 0) scat(b, "+");
    scat(b, t);
    scat(b, unit);
}
static void put_wave(char *b, uint32_t v)       /* the four zones of the wave knob, and the morph inside a zone */
{
    static const char *const NM[4] = {"TRI-SAW", "PULSE", "FOLD", "REED"};
    uint32_t pos = v * 4u, z = pos / 127u, f;
    if (z > 3u) z = 3u;
    f = (pos - z * 127u) * 100u / 127u;
    r_cpy(b, NM[z], 24u);
    scat(b, " ");
    {
        char t[8];
        r_int(t, (int32_t)f);
        scat(b, t);
    }
}
static const char NOTE_NAME[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void put_note(char *b, uint32_t n)
{
    char t[8];
    r_cpy(b, NOTE_NAME[n % 12u], 8u);
    r_int(t, (int32_t)(n / 12u) - 1);
    scat(b, t);
}
static void value_text(unsigned id, unsigned v, char *b)
{
    b[0] = 0;
    switch (id) {
    case RC_P_WAVE_A: case RC_P_WAVE_B: put_wave(b, v); break;
    case RC_P_DETUNE: put_signed(b, ((int32_t)v * 2 - 127) * 100 / 127, " CT"); break;
    case RC_P_ENV_AMT: put_signed(b, ((int32_t)v * 2 - 127) * 100 / 127, " %"); break;
    case RC_P_CUTOFF: put_hz(b, uexp(v, 40u, 2107u)); break;
    case RC_P_FILT_MODE: { uint32_t m = v * 3u / 127u; r_cpy(b, m == 0u ? "LOW-PASS" : m == 1u ? "BAND-PASS" : "HIGH-PASS", 24u); break; }
    case RC_P_DLY_TIME: put_ms(b, 20u + v * 460u / 127u); break;
    case RC_P_DLY_FDBK: put_pct(b, v * 95u / 127u); break;
    case RC_P_DLY_DAMP: put_hz(b, uexp(v, 800u, 894u)); break;
    case RC_P_ATTACK: put_ms(b, uexp(v, 1u, 2807u)); break;
    case RC_P_DECAY: put_ms(b, uexp(v, 5u, 2469u)); break;
    case RC_P_RELEASE: put_ms(b, uexp(v, 5u, 2619u)); break;
    case RC_P_SEQ_GATE: put_pct(b, 10u + v * 90u / 127u); break;
    case RC_P_SEQ_TEMPO: r_int(b, (int32_t)(60u + (v * 140u + 63u) / 127u)); scat(b, " BPM"); break;
    case RC_P_SEQ_MODE: { static const char *const M[4] = {"FORWARD", "REVERSE", "PING-PONG", "RANDOM"}; uint32_t m = v * 4u / 127u; r_cpy(b, M[m > 3u ? 3u : m], 24u); break; }
    case RC_P_SEQ_SWING: put_pct(b, v * 50u / 127u); break;
    default: put_pct(b, v * 100u / 127u); break;   /* osc mix, res, wow, flutter, saturation, hiss, mix, sustain */
    }
}

/* ---- the screen: tape-deck colours */
#define C_BG RGB(0, 0, 0)
#define C_CREAM RGB(236, 226, 196)
#define C_AMBER RGB(232, 150, 64)
#define C_HOT RGB(255, 206, 120)
#define C_MUTED RGB(150, 130, 104)
#define C_TRACK RGB(48, 40, 34)
#define C_CELL RGB(72, 60, 48)
static const char *const PAGE_TAG[NPAGE] = {"P1 OSC", "P2 FILT", "P3 TAPE", "P4 DLY", "P5 ENV", "P6 SEQ"};

static void draw_header(uint32_t page)
{
    cv_begin(240, 24, C_BG);
    cv_text(8, 3, &FONT_S, "RETCON-1", C_AMBER);
    cv_text(232 - text_w(&FONT_S, PAGE_TAG[page]), 3, &FONT_S, PAGE_TAG[page], C_CREAM);
    cv_rect(8, 22, 224, 1, C_TRACK);
    cv_blit(0, 0);
}
static void draw_row(uint32_t r, uint32_t y0, uint32_t id)
{
    char t[40];
    value_text(id, val[id], t);
    cv_begin(240, 40, C_BG);
    cv_text(8, 2, &FONT_S, RC_PARAM_NAME[id], C_MUTED);
    cv_text(232 - text_w(&FONT_S, t), 2, &FONT_S, t, C_CREAM);
    cv_rect(8, 24, 224, 6, C_TRACK);
    cv_rect(8, 24, (int32_t)((uint32_t)val[id] * 224u / 127u), 6, C_AMBER);
    cv_blit(0, y0 + r * 46u);
}
static void draw_steps(void)                    /* the 16 steps: lit = on, ring = playing, white frame = selected */
{
    uint32_t s;
    uint32_t pos = seq_run_pub ? seq_pos_pub : 255u;
    cv_begin(240, 92, C_BG);
    for (s = 0; s < RC_SEQ_STEPS; s++) {
        int32_t x = 8 + (int32_t)(s & 7u) * 28, y = (int32_t)(s >> 3) * 46;
        int on = (int)(set.on >> s & 1u);
        char t[8];
        cv_rect(x, y, 25, 42, on ? C_AMBER : C_CELL);
        if (s == pos) cv_rect(x, y + 36, 25, 6, C_HOT);
        if (s == sel_step) {
            cv_rect(x - 1, y - 1, 27, 1, C_WHITE); cv_rect(x - 1, y + 42, 27, 1, C_WHITE);
            cv_rect(x - 1, y - 1, 1, 44, C_WHITE); cv_rect(x + 25, y - 1, 1, 44, C_WHITE);
        }
        put_note(t, set.note[s]);
        cv_text(x + (25 - text_w(&FONT_S, t)) / 2, y + 12, &FONT_S, t, on ? C_BG : C_MUTED);
    }
    cv_blit(0, 28);
}
static void draw_footer(uint32_t dsp)
{
    char t[40], u[12];
    t[0] = 0;
    scat(t, "DSP ");
    r_int(u, (int32_t)dsp);
    scat(t, u);
    scat(t, "%");
    if (seqmode) {
        static const char *const MD[4] = {"FWD", "REV", "PING", "RAND"};
        uint32_t m = val[RC_P_SEQ_MODE] * 4u / 127u;
        scat(t, " "); scat(t, MD[m > 3u ? 3u : m]);
        scat(t, " SW "); r_int(u, (int32_t)(val[RC_P_SEQ_SWING] * 50u / 127u)); scat(t, u);
        scat(t, " STEP "); r_int(u, (int32_t)sel_step + 1); scat(t, u);
    } else { scat(t, "  OCT "); put_signed(u, (int32_t)(int8_t)set.oct, ""); scat(t, u); }
    if (seq_run_pub) scat(t, "  PLAY");
    if (dirty) scat(t, " *");
    t[39] = 0;
    draw_text_box(0, 220, 240, &FONT_S, t, C_MUTED, 1);
}

static void splash(void)
{
    uint32_t i, t0;
    lcd_fill(0, 0, 240, 240, C_BG);
    draw_text_box(0, 84, 240, &FONT_L, "RETCON-1", C_AMBER, 1);
    draw_text_box(0, 130, 240, &FONT_S, "TAPE-DRIFT SYNTH", C_CREAM, 1);
    draw_text_box(0, 150, 240, &FONT_S, "FOR THE M-VAVE FM-1", C_MUTED, 1);
    lcd_sync();
    t0 = fm1_ms;
#ifdef CULTBOX_HOST
    (void)t0; (void)i;
#else
    for (i = 0; i < 4000u && fm1_ms - t0 < 1500u; i++) platform_poll();
#endif
}

static void leds(uint32_t page)
{
    static const uint8_t PAGE_BTN[NPAGE] = {BTN_SEL, BTN_EDIT, BTN_LFO, BTN_FX, BTN_ENV, BTN_GLO};
    uint32_t k, pos = seq_run_pub ? seq_pos_pub : 255u;
    for (k = 0; k < NPAGE; k++) fm1_led_key(PAGE_BTN[k], k == page);
    fm1_led_key(BTN_SEQ, seqmode);
    fm1_led_key(BTN_PLAY, seq_run_pub);
    fm1_led_key(BTN_OCT_DOWN, !seqmode && (int8_t)set.oct < 0);
    fm1_led_key(BTN_OCT_UP, !seqmode && (int8_t)set.oct > 0);
    for (k = 0; k < 27u; k++) {
        int on = 0, glow = 0;
        if (seqmode && k < RC_SEQ_STEPS) {
            on = (int)(set.on >> k & 1u) ^ (k == pos);
            glow = k == sel_step;
        } else if (!seqmode) on = (int)(fm1_in.notes >> k & 1u);
        fm1_led_key(KEY_LED(k), on);
        fm1_led_key_glow(KEY_LED(k), glow);
    }
}

static void save_all(void)
{
    uint32_t i;
    set.magic = 0xB1; set.ver = 1;
    for (i = 0; i < RC_P_COUNT; i++) set.p[i] = val[i];
    settings_save(&set, sizeof set);
    dirty = 0;
}

static void app_main(void)
{
    uint32_t i, k, draw_t = 0, sig = 0, last_sig = ~0u, home_t = 0, home_used = 0, dsp_pct = 0, dsp_t = 0, saved_once = 0;
    set.magic = 0xB1; set.ver = 1; set.page = 0; set.oct = 0;
    defaults_pattern();
    for (i = 0; i < RC_P_COUNT; i++) set.p[i] = RC_DEFAULT[i];
    if (settings_load(&set, sizeof set) != 0 || set.magic != 0xB1 || set.ver != 1 || set.page >= NPAGE || (int8_t)set.oct < -2 || (int8_t)set.oct > 2) {
        set.magic = 0xB1; set.ver = 1; set.page = 0; set.oct = 0;       /* nothing (or something else) saved: start fresh */
        defaults_pattern();
        for (i = 0; i < RC_P_COUNT; i++) set.p[i] = RC_DEFAULT[i];
    }
    for (i = 0; i < RC_P_COUNT; i++) val[i] = set.p[i] > 127u ? 127u : set.p[i];
    sel_step = 0; seqmode = 0; pre_page = set.page;
    splash();
    for (i = 0; i < RC_P_COUNT; i++) push(EV_PARAM, (uint8_t)i, val[i]);
    push_pattern();
    dirty = 0;
    ready = 1;
    lcd_fill(0, 0, 240, 240, C_BG);

    for (;;) {
        uint32_t keys, down, up, b, rel, page = set.page;
        int32_t e, oct = (int8_t)set.oct;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        /* ---- keys */
        keys = fm1_in.notes;
        down = keys & ~prev_keys;
        up = prev_keys & ~keys;
        prev_keys = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) {
                if (seqmode && k < RC_SEQ_STEPS) {              /* keys 1-16 are the steps */
                    set.on ^= (uint16_t)(1u << k);
                    sel_step = (uint8_t)k;
                    push(EV_STEP, (uint8_t)k, (uint8_t)(set.on >> k & 1u));
                    dirty = 1; t_change = fm1_ms;
                } else if (seqmode) {                           /* keys 17-27 set the selected step's pitch and move on */
                    uint32_t n = base + k;
                    if (n > 108u) n = 108u;
                    set.note[sel_step] = (uint8_t)n;
                    push(EV_STEP_NOTE, sel_step, (uint8_t)n);
                    held_note[k] = n;
                    push(EV_NOTE, (uint8_t)n, 100);             /* (an audition) */
                    sel_step = (uint8_t)((sel_step + 1u) & 15u);
                    dirty = 1; t_change = fm1_ms;
                } else {
                    held_note[k] = base + k;
                    push(EV_NOTE, (uint8_t)held_note[k], 100);
                }
            }
            if (up >> k & 1u) {
                if (held_note[k]) push(EV_NOTE, (uint8_t)held_note[k], 0);
                held_note[k] = 0;
            }
        }

        /* ---- buttons */
        b = fm1_input_edges(&rel);
        (void)rel;
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        {
            static const uint8_t PAGE_BTN[NPAGE] = {BTN_SEL, BTN_EDIT, BTN_LFO, BTN_FX, BTN_ENV, BTN_GLO};
            for (k = 0; k < NPAGE; k++)
                if (b & (1u << PAGE_BTN[k])) { set.page = (uint8_t)k; if (seqmode && k != 5u) seqmode = 0; dirty = 1; t_change = fm1_ms; }
        }
        if (b & (1u << BTN_SEQ)) {
            seqmode = (uint8_t)!seqmode;
            if (seqmode) { pre_page = set.page; set.page = 5; }
            else set.page = pre_page;
        }
        if (b & (1u << BTN_PLAY)) push(EV_RUN, (uint8_t)!seq_run_pub, 0);
        if (b & (1u << BTN_SAVE)) { t_change = 0; dirty = 1; }
        if (fm1_in.buttons >> BTN_HOME & 1u) {
            if (!home_t) { home_t = fm1_ms ? fm1_ms : 1u; home_used = 0; }
            if (!home_used && fm1_ms - home_t > 1500u) {        /* held: back to the starting sound */
                home_used = 1;
                defaults_all();
                defaults_pattern();
                push_pattern();
                sel_step = 0;
            }
        } else if (home_t) {
            if (!home_used) push(EV_PANIC, 0, 0);               /* a tap: all notes off */
            home_t = 0;
        }

        /* ---- encoders */
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            int32_t c = (int32_t)set.page + (e > 0 ? 1 : -1);
            set.page = (uint8_t)(c < 0 ? NPAGE - 1u : c >= (int32_t)NPAGE ? 0 : c);
            if (seqmode && set.page != 5u) seqmode = 0;
            dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0 && seqmode)  /* the step selection */
            sel_step = (uint8_t)(((int32_t)sel_step + (e > 0 ? 1 : 15)) & 15);
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0 && seqmode) {  /* the selected step's pitch, a semitone a click */
            int32_t n = (int32_t)set.note[sel_step] + e;
            n = n < 24 ? 24 : n > 108 ? 108 : n;
            set.note[sel_step] = (uint8_t)n;
            push(EV_STEP_NOTE, sel_step, (uint8_t)n);
            dirty = 1; t_change = fm1_ms;
        }
        page = set.page;
        for (k = 0; k < 4u; k++) {
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                unsigned id = page * 4u + k;
                int32_t ae = e < 0 ? -e : e, step, nv;
                if (id == RC_P_FILT_MODE) step = e > 0 ? 43 : -43;          /* a choice: one click, one choice */
                else if (id == RC_P_SEQ_MODE) step = e > 0 ? 32 : -32;
                else {                                                      /* by speed: slow = 1 a click, spun = up to 8 */
                    uint32_t dt = fm1_ms - knob_t[k], m = dt > 160u ? 1u : dt > 90u ? 2u : dt > 45u ? 4u : 8u;
                    if (ae >= 2) m = m < 4u ? 4u : 8u;
                    step = e * (int32_t)m;
                }
                knob_t[k] = fm1_ms;
                nv = (int32_t)val[id] + step;
                nv = nv < 0 ? 0 : nv > 127 ? 127 : nv;
                if (nv != (int32_t)val[id]) set_val(id, (uint32_t)nv);
            }
        }

        idle_ok = 1;
        midi_in();
        idle_ok = 0;

        /* ---- screen: only what changed */
        if (fm1_ms - dsp_t > 500u) {
            uint32_t pk = dsp_peak;
            dsp_peak = 0;
            dsp_pct = pk * 100u / 5805u;                                  /* of the 5.8 ms a block has */
            dsp_t = fm1_ms;
        }
        sig = page * 977u + seqmode * 31u + dirty * 7u + seq_run_pub * 13u + dsp_pct / 5u + sel_step * 101u + (uint32_t)(int8_t)set.oct * 17u;
        for (k = 0; k < RC_P_COUNT; k++) sig = sig * 131u + val[k];
        if (seqmode) {
            sig = sig * 131u + set.on;
            for (k = 0; k < RC_SEQ_STEPS; k++) sig = sig * 131u + set.note[k];
            sig = sig * 131u + (seq_run_pub ? seq_pos_pub : 255u);
        }
        if ((sig != last_sig && fm1_ms - draw_t >= 30u) || last_sig == ~0u) {
            last_sig = sig;
            draw_t = fm1_ms;
            idle_ok = 1;
            draw_header(page);
            if (seqmode) {
                draw_steps();
                for (k = 0; k < 2u; k++) draw_row(k, 126u, 20u + k);     /* gate, tempo */
            } else {
                for (k = 0; k < 4u; k++) draw_row(k, 30u, page * 4u + k);
            }
            draw_footer(dsp_pct);
            idle_ok = 0;
        }
        leds(page);

        /* ---- save a change 2 s after it, when nothing sounds (an erase drops the sound for ~50 ms) */
        if (dirty && fm1_ms - t_change > 2000u && fm1_ms - loud_ms > 700u && (!saved_once || fm1_ms - t_change > 2000u)) {
            save_all();
            saved_once = 1;
        }
    }
}
