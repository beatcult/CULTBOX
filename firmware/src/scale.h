/* SPDX-License-Identifier: GPL-3.0-only */
/* Scales: twelve of them, from a root. Integer only; the main loop and the audio interrupt both use it. */
#ifndef FM1_SCALE_H
#define FM1_SCALE_H
#include <stdint.h>

#define SCALE_N 12
/* bit n: the note n semitones above the root is in the scale */
static const uint16_t SCALE_MASK[SCALE_N] = {0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x5AB, 0xAD5, 0x6B5, 0x9AD, 0x295, 0x4A9, 0x4E9, 0x555};
static const char *const SCALE_NAMES[SCALE_N] = {"Chroma", "Major", "Minor", "Dorian", "Phryg", "Lydian", "Mixo", "HarmMn", "PentMj", "PentMn", "Blues", "Whole"};
static const char *const ROOT_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static inline int scale_has(unsigned root, unsigned scale, int note)
{
    return (SCALE_MASK[scale < SCALE_N ? scale : 0u] >> (unsigned)((note + 120 - (int)root) % 12)) & 1u;
}
/* the nearest note in the scale (an out-of-scale note goes down, a tie too); scale 0 (chromatic) leaves it alone */
static inline unsigned scale_snap(unsigned root, unsigned scale, unsigned note)
{
    int d, n = (int)note;
    if (scale == 0u || scale >= SCALE_N) return note;
    for (d = 0; d <= 6; d++) {
        if (n - d >= 0 && scale_has(root, scale, n - d)) return (unsigned)(n - d);
        if (n + d <= 127 && scale_has(root, scale, n + d)) return (unsigned)(n + d);
    }
    return note;
}
/* the next note of the scale above (dir > 0) or below (dir < 0) a note, in or out of the scale; at the ends it stays */
static inline unsigned scale_step(unsigned root, unsigned scale, unsigned note, int dir)
{
    int n = (int)note;
    if (scale == 0u || scale >= SCALE_N) { n += dir > 0 ? 1 : -1; return (unsigned)(n < 0 ? 0 : n > 127 ? 127 : n); }
    for (;;) {
        n += dir > 0 ? 1 : -1;
        if (n < 0 || n > 127) return note;
        if (scale_has(root, scale, n)) return (unsigned)n;
    }
}
#endif
