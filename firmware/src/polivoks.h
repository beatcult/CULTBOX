/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Formanta Polivoks voice for the FM-1, built the way the instrument is built.
 *
 * Two oscillators (five footages 32'..2', five shapes, oscillator II fine-tunes and can frequency-modulate
 * oscillator I), a mixer (oscillator levels and noise), ONE resonant 12 dB filter (low-pass or band-pass,
 * no key tracking) and ONE amplifier, each with its own attack / decay / pedestal / after-sound envelope that
 * can repeat, and one modulator (triangle, square, noise, stepped) with its own depth for oscillator I,
 * oscillator II, the filter and the amplifier. In duophonic mode the lowest held key plays oscillator I and the
 * highest plays oscillator II; both notes go through the same filter and amplifier, which is where the grit is.
 * GATE holds the last note with the envelopes running. Glide acts on oscillator I only.
 *
 * Output is mono signed Q23 (full scale 8388608), added to the caller's mix. Audio-interrupt safe: no libc, no
 * allocation. Control-side calls must not overlap the audio interrupt (the app queues them). */
#ifndef FM1_POLIVOKS_H
#define FM1_POLIVOKS_H
#include <stddef.h>
#include <stdint.h>
typedef struct { const char *name; uint8_t max, def; const char *const *names; } fm1_param_t;

#define POLIVOKS_FRAMES 128

/* Parameter pages of four, in KNOB 1-4 order. */
enum {
    POLIVOKS_CUTOFF, POLIVOKS_RESO, POLIVOKS_MODE, POLIVOKS_ENVAMT,           /* 1 filter */
    POLIVOKS_RANGE1, POLIVOKS_WAVE1, POLIVOKS_LEVEL1, POLIVOKS_FM,            /* 2 oscillator I; FM = II -> I */
    POLIVOKS_RANGE2, POLIVOKS_WAVE2, POLIVOKS_LEVEL2, POLIVOKS_DETUNE,        /* 3 oscillator II */
    POLIVOKS_ATTACK, POLIVOKS_DECAY, POLIVOKS_SUSTAIN, POLIVOKS_RELEASE,      /* 4 amplifier envelope */
    POLIVOKS_FATTACK, POLIVOKS_FDECAY, POLIVOKS_FSUSTAIN, POLIVOKS_FRELEASE,  /* 5 filter envelope */
    POLIVOKS_LFORATE, POLIVOKS_LFOSHAPE, POLIVOKS_LFO_OSC1, POLIVOKS_LFO_OSC2,/* 6 modulator */
    POLIVOKS_LFO_FILTER, POLIVOKS_LFO_AMP, POLIVOKS_NOISE, POLIVOKS_DRIVE,    /* 7 modulator to filter / amp, noise, drive */
    POLIVOKS_VOICES, POLIVOKS_GATE, POLIVOKS_GLIDE, POLIVOKS_LOOP,            /* 8 play */
    POLIVOKS_NPARAMS
};
#define POLIVOKS_PAGES (POLIVOKS_NPARAMS / 4)
enum { POLIVOKS_MODE_LP, POLIVOKS_MODE_BP };
enum { POLIVOKS_WAVE_TRI, POLIVOKS_WAVE_SAW, POLIVOKS_WAVE_PULSE50, POLIVOKS_WAVE_PULSE30, POLIVOKS_WAVE_PULSE12 };
enum { POLIVOKS_LFO_TRI, POLIVOKS_LFO_SQR, POLIVOKS_LFO_RANDOM, POLIVOKS_LFO_NOISE };
enum { POLIVOKS_LOOP_OFF, POLIVOKS_LOOP_FILTER, POLIVOKS_LOOP_AMP, POLIVOKS_LOOP_BOTH };
enum { POLIVOKS_MONO, POLIVOKS_DUO, POLIVOKS_UNISON };   /* UNISON: as Mono (the last key on both oscillators), but the detune is spread either side of the note */

#define POLIVOKS_BANK_BYTES 512
typedef struct polivoks_bank polivoks_bank_t;
size_t polivoks_bank_size(void);
void polivoks_bank_init(polivoks_bank_t *b);
void polivoks_bank_panic(polivoks_bank_t *b);              /* silence now, keep parameters */
/* velocity 0 is note-off. The instrument has no velocity: any note-on plays at one level. cents: -100..100. */
void polivoks_bank_note(polivoks_bank_t *b, unsigned note, unsigned velocity, int cents);
/* A note that joins the ones held without retriggering the envelopes, both oscillators gliding to it (a slide; a note after silence still triggers). */
void polivoks_bank_note_legato(polivoks_bank_t *b, unsigned note, unsigned velocity);
void polivoks_bank_pedal(polivoks_bank_t *b, unsigned down);
void polivoks_bank_bend(polivoks_bank_t *b, int32_t semitones_q16);
void polivoks_bank_mod(polivoks_bank_t *b, unsigned value); /* 0..127, CC1: adds oscillator and filter modulation */
/* Adds POLIVOKS_FRAMES mono Q23 samples to out. Returns 0 when silent. */
unsigned polivoks_bank_render(polivoks_bank_t *b, int32_t *out);
void polivoks_bank_set(polivoks_bank_t *b, unsigned parameter, unsigned value);
unsigned polivoks_bank_get(const polivoks_bank_t *b, unsigned parameter);
const fm1_param_t *polivoks_param(int parameter);
unsigned polivoks_bank_active(const polivoks_bank_t *b);
#endif
