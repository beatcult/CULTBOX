/* SPDX-License-Identifier: GPL-3.0-only */
/* CULTBOX DUBMARINE: the firmware's dub siren (firmware/src/dubsiren.c, after beatcult/dubmarine) for the CLAP wrapper
 * (cultbox_clap.c). Mono: the keys play the pitch from Tune (middle C is Tune itself) with a glide; the latest held key wins.
 * Hold latches the siren on; Decay is the release the echo rings out of. Pitch bend +-2 semitones, CC1 deepens the LFO,
 * CC120/123 all notes off. The echo is on from the start (Mix 50), as a dub siren wants it. */
#include "cultbox_clap.h"
#include "dubsiren.h"

static const char *const page_names[3] = {"Pitch and LFO", "Filter", "Play"};
static const char *const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, 0};
static const clap_plugin_descriptor_t desc = {
    CLAP_VERSION_INIT, "com.beatcult.cultbox.dubmarine", "CULTBOX DUBMARINE", "beatcult", "https://github.com/beatcult/cultbox", "", "",
    "0.1.0", "A dub siren with a BBD echo, from the CULTBOX firmware for the M-VAVE FM-1, after beatcult/dubmarine (GPL-3.0)", features};

static cb_pinfo_t info(unsigned id)
{
    const fm1_param_t *v = dubsiren_param((int)id);
    return (cb_pinfo_t){v->name, page_names[id / 4], 0, v->max, v->def, v->names};
}
static void load(void) { dubsiren_tables_init(); }          /* the LFO tables, once, before any thread */
static void init(void *b) { dubsiren_init((dubsiren_bank_t *)b); }
static void set_rate(void *b, float sr) { dubsiren_set_rate((dubsiren_bank_t *)b, sr); }
static void set(void *b, unsigned id, unsigned v) { dubsiren_set((dubsiren_bank_t *)b, id, v); }
static void note(void *b, unsigned key, unsigned vel) { dubsiren_note((dubsiren_bank_t *)b, key, vel); }
static void midi(void *b, const uint8_t *d)
{
    unsigned st = d[0] & 0xF0u;
    if (st == 0xE0u) dubsiren_bend((dubsiren_bank_t *)b, cb_bend_q16(d));
    else if (st == 0xB0u) {
        if (d[1] == 1) dubsiren_mod((dubsiren_bank_t *)b, d[2]);
        else if (d[1] == 120 || d[1] == 123) dubsiren_panic((dubsiren_bank_t *)b);
    }
}
static void vintage(void *b, unsigned v) { dubsiren_vintage((dubsiren_bank_t *)b, v); }
static void panic(void *b) { dubsiren_panic((dubsiren_bank_t *)b); }
static unsigned render(void *b, int32_t *out) { return dubsiren_render((dubsiren_bank_t *)b, out); }

const cb_engine_t cb_engine = {&desc, "DUBMARINE", 0x44584243u /* 'CBXD' */, SIREN_NPARAMS, 50, DUBSIREN_BANK_BYTES, load,
                               info, init, set_rate, set, note, midi, vintage, panic, render};
