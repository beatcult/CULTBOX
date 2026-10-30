/* SPDX-License-Identifier: GPL-3.0-only */
/* Polivoks for the M-VAVE FM-1: a duophonic Formanta Polivoks-style voice with a tape / BBD echo and a spring
 * reverb, on the Dinghy platform (platform.c: updates, USB rescue, USB-MIDI, keys, knobs, screen, flash).
 *
 *   keys       play the voice (two notes at once: the newest two held notes sound)
 *   SELECT     the page (nine: filter, oscillators, amp envelope, filter envelope, LFO, loop / mix,
 *              echo, echo 2, spring)
 *   KNOB 1-4   the four values of the page
 *   OCT- OCT+  the octave (kept in flash)
 *   PRESETS    the MIDI channel: OMNI or 1..16 (kept in flash)
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes, pitch bend, mod wheel (CC1), sustain (CC64), all-notes-off (CC123)
 *
 * The voice (polivoks.c) and the effects (polivoks_fx.c) are float DSP: the build asks for the FM-1's float
 * hardware (-mfprev1, tools/build.py). tests/host.c runs this file on a computer. */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#include "platform.c"
#include "polivoks.c"
#include "polivoks_fx.c"

#define NVOICEPARAM POLIVOKS_NPARAMS
#define NPARAM (POLIVOKS_NPARAMS + POLIVOKS_FX_PARAMS)
#define NPAGE (NPARAM / 4)

/* ---- the sound (audio ISR): events from the main loop, one writer each side */
enum { EV_NOTE, EV_PEDAL, EV_BEND, EV_MOD, EV_PARAM, EV_PANIC };
typedef struct { uint8_t type, a, b, pad; int16_t value; } event_t;
enum { RING = 64 };
static event_t ring[RING];
static volatile uint8_t ev_head, ev_tail, ready;
static uint32_t bank_store[POLIVOKS_BANK_BYTES / 4];
static int32_t mono[POLIVOKS_FRAMES], left[POLIVOKS_FRAMES], right[POLIVOKS_FRAMES];
static uint8_t val[NPARAM];                     /* the values on the panel */

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
    else polivoks_fx_set(p - NVOICEPARAM, v);
}

static void app_audio(int32_t *out, uint32_t n)
{
    polivoks_bank_t *bank = (polivoks_bank_t *)bank_store;
    uint32_t i, blk;
    if (!ready) return;
    while (ev_tail != ev_head) {
        const event_t *e = &ring[ev_tail];
        switch (e->type) {
        case EV_NOTE: polivoks_bank_note(bank, e->a, e->b, 0); break;
        case EV_PEDAL: polivoks_bank_pedal(bank, e->a); break;
        case EV_BEND: polivoks_bank_bend(bank, (int32_t)e->value * 16); break;   /* +-2 semitones */
        case EV_MOD: polivoks_bank_mod(bank, e->a); break;
        case EV_PARAM: apply(e->a, e->b); break;
        default: polivoks_bank_panic(bank); break;
        }
        ev_tail = (uint8_t)((ev_tail + 1u) % RING);
    }
    for (blk = 0; blk + POLIVOKS_FRAMES <= n; blk += POLIVOKS_FRAMES) {
        for (i = 0; i < POLIVOKS_FRAMES; i++) mono[i] = 0;
        polivoks_bank_render(bank, mono);
        for (i = 0; i < POLIVOKS_FRAMES; i++) left[i] = right[i] = mono[i];
        polivoks_fx_process(mono, left, right);
        for (i = 0; i < POLIVOKS_FRAMES; i++) {   /* Q23 -> Q15 */
            out[2u * (blk + i)] += left[i] >> 8;
            out[2u * (blk + i) + 1u] += right[i] >> 8;
        }
    }
}

/* ---- parameters: names, ranges, defaults */
static const char *const TYPE_NAMES[] = {"Tape", "BBD"};
static const fm1_param_t FXP[POLIVOKS_FX_PARAMS] = {
    {"Time", 127, 0, 0}, {"Feedbk", 127, 0, 0}, {"Tone", 127, 0, 0}, {"Echo", 127, 0, 0},
    {"Type", 1, 0, TYPE_NAMES}, {"Wobble", 127, 0, 0}, {"Satur", 127, 0, 0}, {"Width", 127, 0, 0},
    {"Decay", 127, 0, 0}, {"Tone", 127, 0, 0}, {"Drip", 127, 0, 0}, {"Spring", 127, 0, 0}};
static const fm1_param_t *param(unsigned p)
{
    return p < NVOICEPARAM ? polivoks_param((int)p) : &FXP[p - NVOICEPARAM];
}
static const char *const PAGE_NAME[NPAGE] = {"FILTER", "OSCILLATORS", "AMP ENVELOPE", "FILTER ENV", "LFO",
                                             "LOOP AND MIX", "ECHO", "ECHO TYPE", "SPRING"};

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
    for (p = 0; p < POLIVOKS_FX_PARAMS; p++) val[NVOICEPARAM + p] = polivoks_fx_default[p];
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

static void put_num(char *s, uint32_t v)
{
    uint32_t i = 0;
    if (v >= 100u) s[i++] = (char)('0' + v / 100u);
    if (v >= 10u) s[i++] = (char)('0' + v / 10u % 10u);
    s[i++] = (char)('0' + v % 10u);
    s[i] = 0;
}
static void draw_row(unsigned row, unsigned p)
{
    const fm1_param_t *d = param(p);
    char s[16];
    int32_t y = 70 + (int32_t)row * 36;
    if (d->names) { const char *n = d->names[val[p]]; uint32_t i = 0; while (n[i] && i < 15u) { s[i] = n[i]; i++; } s[i] = 0; }
    else put_num(s, val[p]);
    draw_text_box(8, y, 100, &FONT_S, d->name, C_GREY, 0);
    draw_text_box(112, y, 120, &FONT_S, s, C_WHITE, 2);
    lcd_fill(8, y + 20, 224, 6, C_TRACK);
    lcd_fill(8, y + 20, (uint32_t)val[p] * 224u / (d->max ? d->max : 1u), 6, C_ACC);
}
static void draw_page(unsigned page)
{
    char s[8] = "1/9";
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 8, 240, &FONT_L, PAGE_NAME[page], C_ACC, 1);
    s[0] = (char)('1' + page);
    draw_text_box(0, 44, 240, &FONT_S, s, C_GREY, 1);
}
static void draw_status(int32_t oct, uint32_t ch, uint32_t src)
{
    char s[32] = "OCT +0  MIDI OMNI";
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    if (ch) { s[13] = 'C'; s[14] = 'H'; s[15] = (char)(ch < 10u ? ' ' : '1'); s[16] = (char)('0' + ch % 10u); }
    if (src) { s[17] = ' '; s[18] = ' '; s[19] = 'U'; s[20] = src == 2u ? 'A' : 'S'; s[21] = src == 2u ? 'R' : 'B'; s[22] = src == 2u ? 'T' : 0; s[23] = 0; }
    draw_text_box(0, 216, 240, &FONT_S, s, C_GREY, 1);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
typedef struct { uint8_t magic, page, oct, ch; uint8_t p[NPARAM]; } settings_t;
static settings_t set = {0xA5, 0, 0, 0, {0}};
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

static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0, page_drawn = ~0u, status_drawn = ~0u, p;
    uint8_t held[27] = {0};
    uint8_t drawn[4];
    polivoks_bank_t *bank = (polivoks_bank_t *)bank_store;

    defaults();
    if (settings_load(&set, sizeof set) || set.magic != 0xA5 || (int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u || set.page >= NPAGE) {
        set.magic = 0xA5; set.page = 0; set.oct = 0; set.ch = 0;
    } else {
        for (p = 0; p < NPARAM; p++) if (set.p[p] <= param(p)->max) val[p] = set.p[p];
    }
    polivoks_bank_init(bank);                   /* before the audio interrupt may touch them */
    polivoks_fx_reset();
    for (p = 0; p < NPARAM; p++) apply(p, val[p]);
    ready = 1;

    for (;;) {
        uint32_t keys, down, up, rel, b, out_ch = set.ch ? set.ch - 1u : 0u, st, page = set.page;
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
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            int32_t c = (int32_t)page + (e > 0 ? 1 : -1);
            page = (uint32_t)(c < 0 ? NPAGE - 1 : c >= (int32_t)NPAGE ? 0 : c);
            set.page = (uint8_t)page;
            dirty = 1; t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++) {
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                unsigned q = page * 4u + k;
                int32_t step = (e > 2 || e < -2) ? e * 6 : e * 2;
                int32_t v = (int32_t)val[q] + (param(q)->max == 1 ? (e > 0 ? 1 : -1) : step);
                v = v < 0 ? 0 : v > (int32_t)param(q)->max ? (int32_t)param(q)->max : v;
                if ((uint32_t)v != val[q]) { set_param(q, (unsigned)v); dirty = 1; t_change = fm1_ms; }
            }
        }

        midi_in();

        for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), keys >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);

        idle_ok = 1;
        if (page != page_drawn) { page_drawn = page; draw_page(page); for (k = 0; k < 4u; k++) drawn[k] = 0xFF; status_drawn = ~0u; }
        for (k = 0; k < 4u; k++)
            if (drawn[k] != val[page * 4u + k] || ((uint32_t)drawn[k] == 0xFFu)) { drawn[k] = val[page * 4u + k]; draw_row(k, page * 4u + k); }
        if ((st = (uint32_t)(oct + 2) | (uint32_t)set.ch << 4 | midi_src << 12) != status_drawn) { status_drawn = st; draw_status(oct, set.ch, midi_src); }
        idle_ok = 0;

        if (dirty && fm1_ms - t_change > 2000u) {   /* 2 s after the change (an erase stalls the sound; the echo may ring, a short click is possible) */
            for (p = 0; p < NPARAM; p++) set.p[p] = val[p];
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
