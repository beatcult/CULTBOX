/* SPDX-License-Identifier: GPL-3.0-only */
/* CULTBOX PLVKS: the firmware's Polivoks voice (firmware/src/polivoks.c) for the CLAP wrapper (cultbox_clap.c).
 * Duophonic is Voices = Duo: the lowest held key plays oscillator I, the highest oscillator II. Pitch bend +-2 semitones,
 * CC1 modulation, CC64 sustain, CC120/123 all notes off. No velocity: any note plays at one level. */
#include "cultbox_clap.h"
#include "polivoks.h"

static const char *const page_names[POLIVOKS_PAGES] = {"Filter", "Oscillator I", "Oscillator II", "Amp envelope", "Filter envelope", "Modulator", "Modulation and mixer", "Play"};
static const char *const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, 0};
static const clap_plugin_descriptor_t desc = {
    CLAP_VERSION_INIT, "com.beatcult.cultbox.plvks", "CULTBOX PLVKS", "beatcult", "https://github.com/beatcult/cultbox", "", "",
    "0.1.0", "A Formanta Polivoks voice with a BBD echo, from the CULTBOX firmware for the M-VAVE FM-1 (GPL-3.0)", features};

static cb_pinfo_t info(unsigned id)
{
    const fm1_param_t *v = polivoks_param((int)id);
    return (cb_pinfo_t){v->name, page_names[id / 4], 0, v->max, v->def, v->names};
}
static void init(void *b) { polivoks_bank_init((polivoks_bank_t *)b); }
static void set_rate(void *b, float sr) { polivoks_bank_set_rate((polivoks_bank_t *)b, sr); }
static void set(void *b, unsigned id, unsigned v) { polivoks_bank_set((polivoks_bank_t *)b, id, v); }
static void note(void *b, unsigned key, unsigned vel) { polivoks_bank_note((polivoks_bank_t *)b, key, vel, 0); }
static void midi(void *b, const uint8_t *d)
{
    unsigned st = d[0] & 0xF0u;
    if (st == 0xE0u) polivoks_bank_bend((polivoks_bank_t *)b, cb_bend_q16(d));
    else if (st == 0xB0u) {
        if (d[1] == 1) polivoks_bank_mod((polivoks_bank_t *)b, d[2]);
        else if (d[1] == 64) polivoks_bank_pedal((polivoks_bank_t *)b, d[2] >= 64);
        else if (d[1] == 120 || d[1] == 123) polivoks_bank_panic((polivoks_bank_t *)b);
    }
}
static void vintage(void *b, unsigned v) { polivoks_bank_vintage((polivoks_bank_t *)b, v); }
static void panic(void *b) { polivoks_bank_panic((polivoks_bank_t *)b); }
static unsigned render(void *b, int32_t *out) { return polivoks_bank_render((polivoks_bank_t *)b, out); }

const cb_engine_t cb_engine = {&desc, "PLVKS", 0x50584243u /* 'CBXP' */, POLIVOKS_NPARAMS, 0, POLIVOKS_BANK_BYTES, 0,
                               info, init, set_rate, set, note, midi, vintage, panic, render};
