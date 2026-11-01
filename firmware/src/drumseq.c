/* SPDX-License-Identifier: GPL-3.0-only */
/* Drum step sequencer (drumseq.h). */
#include "drumseq.h"

unsigned seq_get(const uint8_t *st, unsigned step, unsigned pad)
{
    const uint8_t *s = st + step * 3u;
    uint32_t v = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16;
    return (v >> (pad * 2u)) & 3u;
}
void seq_put(uint8_t *st, unsigned step, unsigned pad, unsigned state)
{
    uint8_t *s = st + step * 3u;
    uint32_t v = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16;
    v = (v & ~(3u << (pad * 2u))) | (state & 3u) << (pad * 2u);
    s[0] = (uint8_t)v; s[1] = (uint8_t)(v >> 8); s[2] = (uint8_t)(v >> 16);
}

static unsigned steps_per_beat(const seq_t *q) { return q->rate == 0 ? 2u : q->rate == 1u ? 4u : 8u; }
static float step_samples(const seq_t *q)         /* internal: from the tempo; external: from the clocks measured */
{
    unsigned spb = steps_per_beat(q);
    if (q->sync && q->est > 0.0f) return q->est * (float)(24u / spb);
    return 44100.0f * 60.0f / ((float)(q->tempo + 40u) * (float)spb);
}
static void fire(seq_t *q, const uint8_t *steps, drums_bank_t *d, uint32_t at)   /* play the next step, at samples from now */
{
    unsigned len = 16u * ((unsigned)q->len + 1u), n = q->pos == SEQ_NONE ? 0u : (q->pos + 1u) % len, pad;
    uint32_t delay = at;
    if (n & 1u) delay += (uint32_t)(((float)q->swing * (1.0f / 127.0f)) * 0.5f * step_samples(q));   /* swing: the odd steps late */
    for (pad = 0; pad < DRUMS_NPAD; pad++) {
        unsigned s = seq_get(steps, n, pad);
        if (s) drums_note_at(d, drums_note_of_pad(pad), s == 2u ? q->acc : q->hit, delay);
    }
    q->pos = (uint8_t)n;
}

void seq_init(seq_t *q)
{
    q->run = 0; q->pos = SEQ_NONE;
    q->tempo = 80; q->swing = 0; q->len = 0; q->rate = 1; q->sync = 0; q->hit = 90; q->acc = 127;
    q->next_in = 0.0f; q->est = 0.0f; q->clk = 0; q->since = 0;
}
void seq_set(seq_t *q, unsigned p, unsigned v)
{
    switch (p) {
    case SEQ_TEMPO: q->tempo = (uint8_t)(v > 200u ? 200u : v); break;
    case SEQ_SWING: q->swing = (uint8_t)v; break;
    case SEQ_LEN: q->len = (uint8_t)(v > 3u ? 3u : v); break;
    case SEQ_RATE: q->rate = (uint8_t)(v > 2u ? 2u : v); break;
    case SEQ_SYNC: q->sync = (uint8_t)v; q->run = 0; q->pos = SEQ_NONE; break;     /* changing the source stops it */
    case SEQ_HIT: q->hit = (uint8_t)(v ? v : 1u); break;
    case SEQ_ACC: q->acc = (uint8_t)(v ? v : 1u); break;
    default: break;
    }
}
void seq_command(seq_t *q, unsigned c)
{
    if (c == SEQ_START) {
        q->pos = SEQ_NONE; q->next_in = 0.0f; q->since = 0;
        q->clk = 24u / steps_per_beat(q) - 1u;       /* external: the first clock plays step 1 */
        q->run = 1;
    } else if (c == SEQ_CONTINUE) {
        q->run = 1;
    } else if (c == SEQ_STOP) {
        q->run = 0; q->pos = SEQ_NONE;
    }
    /* SEQ_CLOCK is handled by seq_clock (needs the pattern and the drums) */
}
static void seq_clock(seq_t *q, const uint8_t *steps, drums_bank_t *d)   /* a MIDI clock arrived (24 a quarter note) */
{
    if (!q->run || !q->sync) return;
    if (q->since > 0u) {                                 /* measure the clock: smooth, the block grid is coarse */
        float m = (float)q->since;
        q->est = q->est > 0.0f ? q->est + (m - q->est) * 0.15f : m;
    }
    q->since = 0;
    if (++q->clk >= 24u / steps_per_beat(q)) { q->clk = 0; fire(q, steps, d, 0); }
}
void seq_clock_in(seq_t *q, const uint8_t *steps, drums_bank_t *d) { seq_clock(q, steps, d); }

void seq_block(seq_t *q, const uint8_t *steps, drums_bank_t *d)
{
    if (!q->run) return;
    if (q->sync) { if (q->since < 100000u) q->since += DRUMS_FRAMES; return; }
    {
        float sl = step_samples(q);
        while (q->next_in < (float)DRUMS_FRAMES) {
            fire(q, steps, d, (uint32_t)q->next_in);
            q->next_in += sl;
        }
        q->next_in -= (float)DRUMS_FRAMES;
    }
}
