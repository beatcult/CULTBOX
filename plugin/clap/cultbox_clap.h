/* SPDX-License-Identifier: GPL-3.0-only */
/* What an engine gives the CULTBOX CLAP wrapper (cultbox_clap.c): the firmware's own voice, behind a handful of calls.
 * The wrapper does the rest, the same for every engine: the host's blocks cut to 128 frames, notes and MIDI, automatable
 * parameters (the engine's own, then the echo's Time / Feedback / Tone / Mix, Sync and Note, Vintage and Level), the BBD echo
 * (its own instance and delay line), tempo, state, and ports. An engine file defines cb_engine; nothing else. */
#ifndef CULTBOX_CLAP_H
#define CULTBOX_CLAP_H
#include <stddef.h>
#include <stdint.h>
#include <clap/clap.h>

typedef struct {
    const char *name, *page;                    /* "Cutoff", "Filter": the host shows PLVKS/Filter */
    double min, max, def;
    const char *const *names;                   /* the value names of a choice, or 0 */
} cb_pinfo_t;

typedef struct {
    const clap_plugin_descriptor_t *desc;
    const char *prefix;                         /* the module path the host groups parameters under: "PLVKS" */
    uint32_t magic;                             /* the state's header, so one engine's state is not loaded into another */
    unsigned nparams;                           /* the engine's own parameters: ids 0 .. nparams - 1 */
    unsigned echo_mix;                          /* the echo's Mix when the plugin is new (0: dry) */
    size_t bank_bytes;                          /* the engine's state, one per instance */
    void (*load)(void);                         /* once, when the plugin file is loaded, before any thread (or 0) */
    cb_pinfo_t (*info)(unsigned id);
    void (*init)(void *bank);
    void (*set_rate)(void *bank, float sample_rate);                 /* after init */
    void (*set)(void *bank, unsigned id, unsigned value);
    void (*note)(void *bank, unsigned key, unsigned velocity);       /* velocity 0: off */
    void (*midi)(void *bank, const uint8_t *data);                   /* bend, controllers: everything but notes */
    void (*vintage)(void *bank, unsigned amount);                    /* 0..127 */
    void (*panic)(void *bank);
    unsigned (*render)(void *bank, int32_t *out);                    /* adds 128 mono Q23 frames to out */
} cb_engine_t;

extern const cb_engine_t cb_engine;             /* defined by the engine's file */

/* the pitch-bend wheel's 14 bits as the engines take it: +-2 semitones, Q16 */
static inline int32_t cb_bend_q16(const uint8_t *midi) { return (int32_t)(((int)(midi[1] | (midi[2] << 7)) - 8192) * 2 * 65536 / 8192); }
#endif
