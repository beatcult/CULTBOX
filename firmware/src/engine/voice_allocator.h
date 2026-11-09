/* SPDX-License-Identifier: GPL-3.0-only */
/* voice_allocator.h - 4-voice manager: same note first, then idle (oldest = round robin), then releasing,
 * then steal-oldest. */
#ifndef RC_VOICE_ALLOCATOR_H
#define RC_VOICE_ALLOCATOR_H
#include "../dsp/voice.h"

typedef struct {
    rc_voice_t voices[RC_POLYPHONY];
    uint32_t counter;
} rc_alloc_t;

static inline void rc_alloc_init(rc_alloc_t *a)
{
    int i;
    a->counter = 0;
    for (i = 0; i < RC_POLYPHONY; i++) rc_voice_init(&a->voices[i], (uint32_t)(i + 1));
}

static inline void rc_alloc_apply(rc_alloc_t *a, const rc_vparams_t *p)
{
    int i;
    for (i = 0; i < RC_POLYPHONY; i++) rc_voice_apply(&a->voices[i], p);
}

static inline int rc_alloc_pick(const rc_alloc_t *a, uint8_t note)
{
    int i, best = -1;
    for (i = 0; i < RC_POLYPHONY; i++)                          /* 1. a voice still holding this note: retrigger it */
        if (a->voices[i].gate && a->voices[i].note == note) return i;
    for (i = 0; i < RC_POLYPHONY; i++)                          /* 2. an idle voice, oldest first */
        if (!rc_voice_active(&a->voices[i]) && (best < 0 || a->voices[i].age < a->voices[best].age)) best = i;
    if (best >= 0) return best;
    for (i = 0; i < RC_POLYPHONY; i++)                          /* 3. the oldest voice already in release */
        if (!a->voices[i].gate && (best < 0 || a->voices[i].age < a->voices[best].age)) best = i;
    if (best >= 0) return best;
    best = 0;                                                   /* 4. steal the oldest held voice */
    for (i = 1; i < RC_POLYPHONY; i++)
        if (a->voices[i].age < a->voices[best].age) best = i;
    return best;
}

static inline void rc_alloc_note_on(rc_alloc_t *a, uint8_t note, float velocity)
{
    rc_voice_note_on(&a->voices[rc_alloc_pick(a, note)], note, velocity, ++a->counter);
}

static inline void rc_alloc_note_off(rc_alloc_t *a, uint8_t note)
{
    int i;
    for (i = 0; i < RC_POLYPHONY; i++)
        if (a->voices[i].gate && a->voices[i].note == note) rc_voice_note_off(&a->voices[i]);
}

static inline void rc_alloc_all_off(rc_alloc_t *a)
{
    int i;
    for (i = 0; i < RC_POLYPHONY; i++)
        if (a->voices[i].gate) rc_voice_note_off(&a->voices[i]);
}

static inline int rc_alloc_active(const rc_alloc_t *a)
{
    int i, n = 0;
    for (i = 0; i < RC_POLYPHONY; i++) n += rc_voice_active(&a->voices[i]) ? 1 : 0;
    return n;
}

#endif
