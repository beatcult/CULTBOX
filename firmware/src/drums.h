/* SPDX-License-Identifier: GPL-3.0-only */
/* Sample drums: ten pads from a kit in flash (drums_data.h), each with tune, decay and level, a global level, crush and
 * tone, and the closed hat chokes the open one. Float DSP; audio interrupt only. Output: mono Q23, added to out. */
#ifndef FM1_DRUMS_H
#define FM1_DRUMS_H
#include <stdint.h>

#define DRUMS_FRAMES 128
#define DRUMS_NPAD 10
enum { DRUM_TUNE, DRUM_DECAY, DRUM_LEVEL, DRUM_PADPARAMS };          /* per pad: 64 = original pitch; 127 = the whole sample; 100 */
#define DRUMS_PADVALS (DRUMS_NPAD * DRUM_PADPARAMS)                  /* parameter p < 30: pad p / 3, value p % 3 */
enum { DRUMS_LEVEL = DRUMS_PADVALS, DRUMS_CRUSH, DRUMS_TONE, DRUMS_CHOKE, DRUMS_END };
typedef struct { const char *name; const int16_t *data; uint32_t len; uint32_t rate; } drum_sample_t;
#define DRUMS_BANK_BYTES 640
typedef struct drums_bank drums_bank_t;
void drums_init(drums_bank_t *b);
void drums_panic(drums_bank_t *b);
void drums_note(drums_bank_t *b, unsigned note, unsigned velocity);   /* a General MIDI drum note; velocity 0: ignored (one shots) */
void drums_note_at(drums_bank_t *b, unsigned note, unsigned velocity, uint32_t delay);   /* the same, starting delay samples from now (the sequencer) */
void drums_note_ex(drums_bank_t *b, unsigned note, unsigned velocity, uint32_t delay, int tune_half);   /* and a pitch offset in half semitones, -24..24 */
void drums_set(drums_bank_t *b, unsigned parameter, unsigned value);
unsigned drums_render(drums_bank_t *b, int32_t *out);                 /* 0 when silent */
int drums_pad_of_note(unsigned note);                                 /* -1: not a pad */
unsigned drums_note_of_pad(unsigned pad);
extern const char *const *const drums_names;                          /* the pad names of the kit */
#endif
