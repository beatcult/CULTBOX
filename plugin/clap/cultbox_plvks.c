/* SPDX-License-Identifier: GPL-3.0-only */
/* CULTBOX PLVKS as a CLAP plugin: the firmware's Polivoks voice (firmware/src/polivoks.c) and its BBD echo
 * (firmware/src/polivoks_fx.c), unchanged, behind a host's sample rate, block size, notes, parameters and state.
 *
 *  - The voice and the echo work in blocks of 128 frames. The host's blocks, of any size, are cut to 128 by a small FIFO;
 *    notes, MIDI and parameter changes are applied at the start of the next 128-frame block (2.7 ms at 48 kHz).
 *  - Parameters: the voice's 32 (by name, with the firmware's value names), the echo's four, its tempo sync and note value,
 *    VINTAGE, and an output level. All are automatable. The echo follows the host's tempo when SYNC is on.
 *  - MIDI: notes, pitch bend (+-2 semitones), CC1 (modulation), CC64 (sustain), CC120/123 (all off).
 *  - One stereo output: the voice in the middle, the echo in stereo.
 * Built with -DPOLIVOKS_FX_NO_LEGACY (the FM-1's two global echoes are not wanted here). */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include "polivoks.h"
#include "polivoks_fx.h"

#define BLK POLIVOKS_FRAMES
#define Q23 8388608.0f

/* parameter ids: the voice's own numbers first, then the rest */
enum {
    P_FX_TIME = POLIVOKS_NPARAMS, P_FX_FEED, P_FX_TONE, P_FX_MIX,   /* the echo */
    P_FX_SYNC, P_FX_NOTE,                                           /* tempo sync, and its note value */
    P_VINTAGE, P_LEVEL, P_COUNT
};
static const char *const onoff[] = {"Off", "On"};
static const char *const note_names[POLIVOKS_FX_NOTES] = {"1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T", "1/4.", "1/2"};
static const char *const page_names[POLIVOKS_PAGES] = {"Filter", "Oscillator I", "Oscillator II", "Amp envelope", "Filter envelope", "Modulator", "Modulation and mixer", "Play"};

typedef struct { const char *name, *module; double min, max, def; const char *const *names; } pinfo_t;
static pinfo_t pinfo(unsigned id)
{
    const fm1_param_t *v;
    if (id < POLIVOKS_NPARAMS) { v = polivoks_param((int)id); return (pinfo_t){v->name, page_names[id / 4], 0, v->max, v->def, v->names}; }
    switch (id) {
    case P_FX_TIME: return (pinfo_t){"Echo Time", "Echo", 0, 127, polivoks_fx_default[FX_DLY_TIME], 0};
    case P_FX_FEED: return (pinfo_t){"Echo Feedback", "Echo", 0, 127, polivoks_fx_default[FX_DLY_FEED], 0};
    case P_FX_TONE: return (pinfo_t){"Echo Tone", "Echo", 0, 127, polivoks_fx_default[FX_DLY_TONE], 0};
    case P_FX_MIX: return (pinfo_t){"Echo Mix", "Echo", 0, 127, polivoks_fx_default[FX_DLY_MIX], 0};
    case P_FX_SYNC: return (pinfo_t){"Echo Sync", "Echo", 0, 1, 0, onoff};
    case P_FX_NOTE: return (pinfo_t){"Echo Note", "Echo", 0, POLIVOKS_FX_NOTES - 1, 5, note_names};
    case P_VINTAGE: return (pinfo_t){"Vintage", "Output", 0, 127, 0, 0};
    default: return (pinfo_t){"Level", "Output", -36, 6, 0, 0};               /* dB */
    }
}

typedef struct { uint64_t t; uint32_t kind, a; double b; } qev_t;   /* a note, a MIDI byte triple or a parameter, waiting for the next block */
enum { EV_NOTE_ON, EV_NOTE_OFF, EV_MIDI, EV_PARAM };
#define QN 2048u

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t *host;
    _Atomic float val[P_COUNT];                 /* the parameters as the host sees them (main thread reads, audio thread writes) */
    _Atomic int reload;                         /* a state was loaded: the audio thread applies every parameter */
    uint32_t bank_mem[POLIVOKS_BANK_BYTES / 4];
    polivoks_bank_t *bank;
    polivoks_fx_t fx;
    int16_t *fxbuf;
    double sr, tempo;
    int active;
    float gain;
    int32_t dry[BLK];
    float outl[BLK], outr[BLK];
    unsigned pos;                               /* the next frame of outl/outr to give out; BLK: render another block */
    uint64_t frames_done;
    qev_t q[QN]; unsigned qr, qw;               /* ring of events, in time order */
} plug_t;

/* ---- parameters into the engines (audio thread, or the main thread while the plugin is not active) ---- */
static void apply(plug_t *p, unsigned id)
{
    double v = (double)atomic_load_explicit(&p->val[id], memory_order_relaxed);
    if (id < POLIVOKS_NPARAMS) polivoks_bank_set(p->bank, id, (unsigned)lrint(v));
    else if (!p->active) return;
    else if (id >= P_FX_TIME && id <= P_FX_MIX) polivoks_fx_unit_set(&p->fx, id - P_FX_TIME, (unsigned)lrint(v));
    else if (id == P_FX_SYNC || id == P_FX_NOTE)
        polivoks_fx_unit_sync(&p->fx, (unsigned)lrint((double)atomic_load_explicit(&p->val[P_FX_SYNC], memory_order_relaxed)),
                              (unsigned)lrint((double)atomic_load_explicit(&p->val[P_FX_NOTE], memory_order_relaxed)));
    else if (id == P_VINTAGE) polivoks_bank_vintage(p->bank, (unsigned)lrint(v));
    else if (id == P_LEVEL) p->gain = powf(10.0f, (float)v * 0.05f) * (1.0f / Q23);
}
static void apply_all(plug_t *p) { for (unsigned i = 0; i < P_COUNT; ++i) apply(p, i); }
static void set_param(plug_t *p, unsigned id, double v)
{
    pinfo_t i;
    if (id >= P_COUNT) return;
    i = pinfo(id);
    if (v < i.min) v = i.min;
    if (v > i.max) v = i.max;
    if (id != P_LEVEL) v = floor(v + 0.5);
    atomic_store_explicit(&p->val[id], (float)v, memory_order_relaxed);
    apply(p, id);
}

/* ---- events ---- */
static void midi_event(plug_t *p, const uint8_t *d)
{
    unsigned st = d[0] & 0xF0u;
    if (st == 0x90u && d[2]) polivoks_bank_note(p->bank, d[1], d[2], 0);
    else if (st == 0x80u || st == 0x90u) polivoks_bank_note(p->bank, d[1], 0, 0);
    else if (st == 0xE0u) polivoks_bank_bend(p->bank, (int32_t)(((int)(d[1] | (d[2] << 7)) - 8192) * 2 * 65536 / 8192));   /* +-2 semitones */
    else if (st == 0xB0u) {
        if (d[1] == 1) polivoks_bank_mod(p->bank, d[2]);
        else if (d[1] == 64) polivoks_bank_pedal(p->bank, d[2] >= 64);
        else if (d[1] == 120 || d[1] == 123) polivoks_bank_panic(p->bank);
    }
}
static void run_event(plug_t *p, const qev_t *e)
{
    if (e->kind == EV_NOTE_ON) polivoks_bank_note(p->bank, e->a, (unsigned)e->b, 0);
    else if (e->kind == EV_NOTE_OFF) polivoks_bank_note(p->bank, e->a, 0, 0);
    else if (e->kind == EV_MIDI) { uint8_t d[3] = {(uint8_t)e->a, (uint8_t)(e->a >> 8), (uint8_t)(e->a >> 16)}; midi_event(p, d); }
    else set_param(p, e->a, e->b);
}
static void queue(plug_t *p, uint64_t t, uint32_t kind, uint32_t a, double b)
{
    qev_t e = {t, kind, a, b};
    if (p->qw - p->qr >= QN) { run_event(p, &p->q[p->qr % QN]); ++p->qr; }   /* full: the oldest goes in now */
    p->q[p->qw % QN] = e; ++p->qw;
}
static void take(plug_t *p, const clap_event_header_t *h, uint64_t base)
{
    uint64_t t = base + h->time;
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
        const clap_event_note_t *n = (const clap_event_note_t *)h;
        if (n->key >= 0 && n->key < 128) { int v = (int)lrint(n->velocity * 127.0); queue(p, t, EV_NOTE_ON, (uint32_t)n->key, v < 1 ? 1 : v > 127 ? 127 : v); }
        break; }
    case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: {
        const clap_event_note_t *n = (const clap_event_note_t *)h;
        if (n->key >= 0 && n->key < 128) queue(p, t, EV_NOTE_OFF, (uint32_t)n->key, 0);
        break; }
    case CLAP_EVENT_MIDI: {
        const clap_event_midi_t *m = (const clap_event_midi_t *)h;
        queue(p, t, EV_MIDI, m->data[0] | (m->data[1] << 8) | (m->data[2] << 16), 0);
        break; }
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
        queue(p, t, EV_PARAM, e->param_id, e->value);
        break; }
    default: break;
    }
}

/* ---- audio ---- */
static void render_block(plug_t *p)
{
    unsigned i;
    memset(p->dry, 0, sizeof p->dry);
    polivoks_bank_render(p->bank, p->dry);
    {
        int32_t l[BLK], r[BLK];
        memcpy(l, p->dry, sizeof l); memcpy(r, p->dry, sizeof r);
        if (p->tempo > 20.0) polivoks_fx_unit_tempo(&p->fx, (float)(p->sr * 60.0 / p->tempo));
        polivoks_fx_unit_process(&p->fx, p->dry, l, r);
        for (i = 0; i < BLK; ++i) { p->outl[i] = (float)l[i] * p->gain; p->outr[i] = (float)r[i] * p->gain; }
    }
}
static clap_process_status process(const clap_plugin_t *pl, const clap_process_t *pr)
{
    plug_t *p = (plug_t *)pl->plugin_data;
    uint32_t n = pr->frames_count, nev = pr->in_events ? pr->in_events->size(pr->in_events) : 0, i;
    float *L, *R;
    if (!pr->audio_outputs_count || pr->audio_outputs[0].channel_count < 1 || !pr->audio_outputs[0].data32) return CLAP_PROCESS_ERROR;
    L = pr->audio_outputs[0].data32[0];
    R = pr->audio_outputs[0].channel_count > 1 ? pr->audio_outputs[0].data32[1] : L;
    if (atomic_exchange_explicit(&p->reload, 0, memory_order_acquire)) apply_all(p);
    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && pr->transport->tempo > 20.0) p->tempo = pr->transport->tempo;
    for (i = 0; i < nev; ++i) take(p, pr->in_events->get(pr->in_events, i), p->frames_done);
    for (i = 0; i < n; ++i) {
        if (p->pos >= BLK) {                                       /* a new block: the events due by now go in first */
            while (p->qr != p->qw && p->q[p->qr % QN].t <= p->frames_done + i) { run_event(p, &p->q[p->qr % QN]); ++p->qr; }
            render_block(p);
            p->pos = 0;
        }
        L[i] = p->outl[p->pos]; if (R != L) R[i] = p->outr[p->pos];   /* (a host that gave one channel gets the left) */
        ++p->pos;
    }
    p->frames_done += n;
    return CLAP_PROCESS_CONTINUE;
}
static void flush_params(const clap_plugin_t *pl, const clap_input_events_t *in, const clap_output_events_t *out)
{
    plug_t *p = (plug_t *)pl->plugin_data;
    (void)out;
    for (uint32_t i = 0; in && i < in->size(in); ++i) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
            set_param(p, ((const clap_event_param_value_t *)h)->param_id, ((const clap_event_param_value_t *)h)->value);
    }
}

/* ---- the plugin ---- */
static void clear(plug_t *p)
{
    polivoks_bank_panic(p->bank);
    p->pos = BLK; p->qr = p->qw = 0; p->frames_done = 0;
    if (p->active) polivoks_fx_unit_init(&p->fx, p->fxbuf, polivoks_fx_buffer_len((float)p->sr), (float)p->sr, 0);
    apply_all(p);
}
static bool plug_init(const clap_plugin_t *pl)
{
    plug_t *p = (plug_t *)pl->plugin_data;
    p->bank = (polivoks_bank_t *)p->bank_mem;
    polivoks_bank_init(p->bank);
    for (unsigned i = 0; i < P_COUNT; ++i) atomic_store(&p->val[i], (float)pinfo(i).def);
    p->sr = 44100.0; p->tempo = 120.0; p->pos = BLK; p->gain = 1.0f / Q23;
    return true;
}
static void plug_destroy(const clap_plugin_t *pl) { free(pl->plugin_data); }
static bool plug_activate(const clap_plugin_t *pl, double sr, uint32_t min_frames, uint32_t max_frames)
{
    plug_t *p = (plug_t *)pl->plugin_data;
    unsigned n = polivoks_fx_buffer_len((float)sr);
    (void)min_frames; (void)max_frames;
    free(p->fxbuf);
    p->fxbuf = (int16_t *)calloc(n, sizeof(int16_t));
    if (!p->fxbuf) return false;
    p->sr = sr;
    polivoks_bank_set_rate(p->bank, (float)sr);
    p->active = 1;
    clear(p);
    return true;
}
static void plug_deactivate(const clap_plugin_t *pl)
{
    plug_t *p = (plug_t *)pl->plugin_data;
    p->active = 0;
    free(p->fxbuf); p->fxbuf = 0;
}
static bool plug_start(const clap_plugin_t *pl) { (void)pl; return true; }
static void plug_stop(const clap_plugin_t *pl) { (void)pl; }
static void plug_reset(const clap_plugin_t *pl) { if (((plug_t *)pl->plugin_data)->active) clear((plug_t *)pl->plugin_data); }
static void plug_main(const clap_plugin_t *pl) { (void)pl; }

/* params */
static uint32_t pcount(const clap_plugin_t *pl) { (void)pl; return P_COUNT; }
static bool pget_info(const clap_plugin_t *pl, uint32_t i, clap_param_info_t *o)
{
    pinfo_t d;
    (void)pl;
    if (i >= P_COUNT) return false;
    d = pinfo(i);
    memset(o, 0, sizeof *o);
    o->id = i;
    o->flags = CLAP_PARAM_IS_AUTOMATABLE | (i == P_LEVEL ? 0 : CLAP_PARAM_IS_STEPPED) | (d.names ? CLAP_PARAM_IS_ENUM : 0);
    snprintf(o->name, sizeof o->name, "%s", d.name);
    snprintf(o->module, sizeof o->module, "PLVKS/%s", d.module);
    o->min_value = d.min; o->max_value = d.max; o->default_value = d.def;
    return true;
}
static bool pget_value(const clap_plugin_t *pl, clap_id id, double *v)
{
    if (id >= P_COUNT) return false;
    *v = (double)atomic_load_explicit(&((plug_t *)pl->plugin_data)->val[id], memory_order_relaxed);
    return true;
}
static bool ptext(const clap_plugin_t *pl, clap_id id, double v, char *out, uint32_t n)
{
    pinfo_t d;
    (void)pl;
    if (id >= P_COUNT) return false;
    d = pinfo(id);
    if (d.names) { long k = lrint(v); if (k < 0 || k > (long)d.max) return false; snprintf(out, n, "%s", d.names[k]); }
    else if (id == P_LEVEL) snprintf(out, n, "%.1f dB", v);
    else snprintf(out, n, "%ld", lrint(v));
    return true;
}
static bool pfromtext(const clap_plugin_t *pl, clap_id id, const char *text, double *out)
{
    pinfo_t d;
    char *end;
    (void)pl;
    if (id >= P_COUNT) return false;
    d = pinfo(id);
    if (d.names) for (long k = 0; k <= (long)d.max; ++k) if (!strcmp(text, d.names[k])) { *out = (double)k; return true; }
    *out = strtod(text, &end);
    return end != text;
}
static const clap_plugin_params_t params_ext = {pcount, pget_info, pget_value, ptext, pfromtext, flush_params};

/* ports */
static uint32_t aports_count(const clap_plugin_t *pl, bool is_input) { (void)pl; return is_input ? 0 : 1; }
static bool aports_get(const clap_plugin_t *pl, uint32_t i, bool is_input, clap_audio_port_info_t *o)
{
    (void)pl;
    if (is_input || i) return false;
    memset(o, 0, sizeof *o);
    o->id = 0; snprintf(o->name, sizeof o->name, "Out");
    o->flags = CLAP_AUDIO_PORT_IS_MAIN; o->channel_count = 2; o->port_type = CLAP_PORT_STEREO; o->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t aports_ext = {aports_count, aports_get};
static uint32_t nports_count(const clap_plugin_t *pl, bool is_input) { (void)pl; return is_input ? 1 : 0; }
static bool nports_get(const clap_plugin_t *pl, uint32_t i, bool is_input, clap_note_port_info_t *o)
{
    (void)pl;
    if (!is_input || i) return false;
    memset(o, 0, sizeof *o);
    o->id = 0; o->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; o->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    snprintf(o->name, sizeof o->name, "Notes");
    return true;
}
static const clap_plugin_note_ports_t nports_ext = {nports_count, nports_get};

/* the echo's repeats die away in well under ten seconds at any setting */
static uint32_t tail_get(const clap_plugin_t *pl) { return (uint32_t)(10.0 * ((plug_t *)pl->plugin_data)->sr); }
static const clap_plugin_tail_t tail_ext = {tail_get};

/* state: 'CBXP', version 1, then every parameter as a float */
static bool state_save(const clap_plugin_t *pl, const clap_ostream_t *s)
{
    plug_t *p = (plug_t *)pl->plugin_data;
    uint32_t head[2] = {0x50584243u, 1u};
    float v[P_COUNT];
    for (unsigned i = 0; i < P_COUNT; ++i) v[i] = atomic_load_explicit(&p->val[i], memory_order_relaxed);
    return s->write(s, head, sizeof head) == (int64_t)sizeof head && s->write(s, v, sizeof v) == (int64_t)sizeof v;
}
static bool read_all(const clap_istream_t *s, void *buf, size_t n)
{
    char *c = (char *)buf;
    while (n) { int64_t r = s->read(s, c, n); if (r <= 0) return false; c += r; n -= (size_t)r; }
    return true;
}
static bool state_load(const clap_plugin_t *pl, const clap_istream_t *s)
{
    plug_t *p = (plug_t *)pl->plugin_data;
    uint32_t head[2];
    float v[P_COUNT];
    if (!read_all(s, head, sizeof head) || head[0] != 0x50584243u || head[1] != 1u || !read_all(s, v, sizeof v)) return false;
    for (unsigned i = 0; i < P_COUNT; ++i) {
        pinfo_t d = pinfo(i);
        double x = v[i] != v[i] ? d.def : v[i];
        atomic_store_explicit(&p->val[i], (float)(x < d.min ? d.min : x > d.max ? d.max : x), memory_order_relaxed);
    }
    if (p->active) atomic_store_explicit(&p->reload, 1, memory_order_release);   /* (the audio thread applies it) */
    else apply_all(p);
    return true;
}
static const clap_plugin_state_t state_ext = {state_save, state_load};

static const void *plug_ext(const clap_plugin_t *pl, const char *id)
{
    (void)pl;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &params_ext;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &aports_ext;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &nports_ext;
    if (!strcmp(id, CLAP_EXT_STATE)) return &state_ext;
    if (!strcmp(id, CLAP_EXT_TAIL)) return &tail_ext;
    return 0;
}

/* ---- factory ---- */
static const char *const features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, 0};
static const clap_plugin_descriptor_t desc = {
    CLAP_VERSION_INIT, "com.beatcult.cultbox.plvks", "CULTBOX PLVKS", "beatcult", "https://github.com/beatcult/cultbox", "", "",
    "0.1.0", "A Formanta Polivoks voice with a BBD echo, from the CULTBOX firmware for the M-VAVE FM-1 (GPL-3.0)", features};

static uint32_t fac_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *fac_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i ? 0 : &desc; }
static const clap_plugin_t *fac_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
    plug_t *p;
    (void)f;
    if (!id || strcmp(id, desc.id)) return 0;
    p = (plug_t *)calloc(1, sizeof *p);
    if (!p) return 0;
    p->host = host;
    p->plugin = (clap_plugin_t){&desc, p, plug_init, plug_destroy, plug_activate, plug_deactivate, plug_start, plug_stop, plug_reset, process, plug_ext, plug_main};
    return &p->plugin;
}
static const clap_plugin_factory_t factory = {fac_count, fac_desc, fac_create};
static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void *entry_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : 0; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
