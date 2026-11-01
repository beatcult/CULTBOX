/* SPDX-License-Identifier: GPL-3.0-only */
/* Step sequencer (drumseq.h). */
#include "drumseq.h"

/* ---- the drums' steps: 3 bytes a step */
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

/* ---- the drums' edited hits: 4 bytes each, after the steps. bit 31 used; 0-5 step, 6-9 pad, 10-15 tune, 16-22 velocity, 23-26 delay, 27-28 ratchet */
static uint32_t ex_read(const uint8_t *e) { return (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24; }
static int ex_find(const uint8_t *pat, unsigned step, unsigned pad)
{
    unsigned i;
    for (i = 0; i < SEQ_NEX; i++) {
        uint32_t v = ex_read(pat + SEQ_DR_BYTES + i * 4u);
        if ((v >> 31) && (v & 63u) == step && (v >> 6 & 15u) == pad) return (int)i;
    }
    return -1;
}
static uint32_t ex_pack(unsigned step, unsigned pad, const seq_hit_t *h)
{
    return 1u << 31 | (step & 63u) | (pad & 15u) << 6 | (uint32_t)(h->tune > 48u ? 48u : h->tune) << 10 | (uint32_t)(h->vel & 127u) << 16 |
           (uint32_t)(h->delay & 15u) << 23 | (uint32_t)(h->ratchet & 3u) << 27;
}
int seq_hit_get(const uint8_t *pat, unsigned step, unsigned pad, seq_hit_t *h)
{
    int i = ex_find(pat, step, pad);
    uint32_t v;
    if (i < 0) return 0;
    v = ex_read(pat + SEQ_DR_BYTES + (unsigned)i * 4u);
    h->tune = (uint8_t)(v >> 10 & 63u); h->vel = (uint8_t)(v >> 16 & 127u); h->delay = (uint8_t)(v >> 23 & 15u); h->ratchet = (uint8_t)(v >> 27 & 3u);
    return 1;
}
int seq_hit_put(uint8_t *pat, unsigned step, unsigned pad, const seq_hit_t *h)
{
    int i = ex_find(pat, step, pad);
    unsigned k;
    uint32_t v = ex_pack(step, pad, h);
    uint8_t *e;
    if (i < 0) {
        for (k = 0; k < SEQ_NEX; k++) if (!(pat[SEQ_DR_BYTES + k * 4u + 3u] & 0x80u)) { i = (int)k; break; }
        if (i < 0) return -1;
    }
    e = pat + SEQ_DR_BYTES + (unsigned)i * 4u;
    e[0] = (uint8_t)v; e[1] = (uint8_t)(v >> 8); e[2] = (uint8_t)(v >> 16); e[3] = (uint8_t)(v >> 24);   /* the byte with the used flag last */
    return 0;
}
void seq_hit_del(uint8_t *pat, unsigned step, unsigned pad)
{
    int i = ex_find(pat, step, pad);
    if (i >= 0) pat[SEQ_DR_BYTES + (unsigned)i * 4u + 3u] = 0;
}

/* ---- the PLVKS steps: 4 bytes each. b0 on | note 1; b1 has2 | note 2; b2 length | delay << 4; b3 velocity */
void seq_note_get(const uint8_t *pat, unsigned step, seq_note_t *r)
{
    const uint8_t *s = pat + step * 4u;
    r->on = s[0] >> 7; r->n1 = s[0] & 127u; r->has2 = s[1] >> 7; r->n2 = s[1] & 127u;
    r->len = s[2] & 15u; r->delay = s[2] >> 4; r->amp = s[3] & 127u;
}
void seq_note_put(uint8_t *pat, unsigned step, const seq_note_t *r)
{
    uint8_t *s = pat + step * 4u;
    s[3] = (uint8_t)(r->amp & 127u);
    s[2] = (uint8_t)((r->len & 15u) | (r->delay & 15u) << 4);
    s[1] = (uint8_t)((r->n2 & 127u) | (r->has2 ? 0x80u : 0u));
    s[0] = (uint8_t)((r->n1 & 127u) | (r->on ? 0x80u : 0u));
}

void seq_clear(uint8_t *pat) { unsigned i; for (i = 0; i < SEQ_PAT_BYTES; i++) pat[i] = 0; }
void seq_clear_row(uint8_t *pat, unsigned pad)
{
    unsigned i;
    for (i = 0; i < SEQ_STEPS; i++) { seq_put(pat, i, pad, 0); }
    for (i = 0; i < SEQ_NEX; i++) {
        uint32_t v = ex_read(pat + SEQ_DR_BYTES + i * 4u);
        if ((v >> 31) && (v >> 6 & 15u) == pad) pat[SEQ_DR_BYTES + i * 4u + 3u] = 0;
    }
}
void seq_copy_page(uint8_t *pat, unsigned kind, unsigned from, unsigned to)
{
    unsigned i;
    if (from == to || from > 3u || to > 3u) return;
    if (kind == SEQ_KIND_PLVKS) {
        for (i = 0; i < 64u; i++) pat[to * 64u + i] = pat[from * 64u + i];
        return;
    }
    {
        uint32_t keep[SEQ_NEX];
        unsigned n = 0;
        for (i = 0; i < 48u; i++) pat[to * 48u + i] = pat[from * 48u + i];       /* 16 steps x 3 bytes */
        for (i = 0; i < SEQ_NEX; i++) {                                           /* the target page's edits go, the source's are copied */
            uint32_t v = ex_read(pat + SEQ_DR_BYTES + i * 4u);
            if (!(v >> 31)) continue;
            if ((v & 63u) >> 4 == to) pat[SEQ_DR_BYTES + i * 4u + 3u] = 0;
            else if ((v & 63u) >> 4 == from) keep[n++] = v;
        }
        for (i = 0; i < n; i++) {
            seq_hit_t h;
            uint32_t v = keep[i];
            h.tune = (uint8_t)(v >> 10 & 63u); h.vel = (uint8_t)(v >> 16 & 127u); h.delay = (uint8_t)(v >> 23 & 15u); h.ratchet = (uint8_t)(v >> 27 & 3u);
            seq_hit_put(pat, (v & 15u) | to << 4, v >> 6 & 15u, &h);
        }
    }
}

/* ---- the clock */
static unsigned steps_per_beat(const seq_t *q) { return q->rate == 0 ? 2u : q->rate == 1u ? 4u : 8u; }
static float step_samples(const seq_t *q)         /* internal: from the tempo; external: from the clocks measured */
{
    unsigned spb = steps_per_beat(q);
    if (q->sync && q->est > 0.0f) return q->est * (float)(24u / spb);
    return 44100.0f * 60.0f / ((float)(q->tempo + 40u) * (float)spb);
}

/* ---- PLVKS notes: scheduled, quantised to the audio block */
static void snd_mark(seq_t *q, unsigned note, int on)
{
    unsigned i;
    for (i = 0; i < 6u; i++) if (q->snd[i] == note) q->snd[i] = 255;
    if (on) for (i = 0; i < 6u; i++) if (q->snd[i] == 255) { q->snd[i] = (uint8_t)note; break; }
}
static void ev_add(seq_t *q, float t, unsigned note, unsigned vel)
{
    unsigned i;
    if (vel) for (i = 0; i < 8u; i++) if (q->ev[i].used && !q->ev[i].vel && q->ev[i].note == note) q->ev[i].used = 0;   /* a new note cancels its old note-off */
    for (i = 0; i < 8u; i++) if (!q->ev[i].used) {
        q->ev[i].t = t; q->ev[i].note = (uint8_t)note; q->ev[i].vel = (uint8_t)vel; q->ev[i].used = 1;
        return;
    }
}
static void ev_flush(seq_t *q)                   /* what is due before the next block happens now; the rest moves up */
{
    unsigned i;
    for (i = 0; i < 8u; i++) {
        seq_ev_t *e = &q->ev[i];
        if (!e->used) continue;
        if (e->t < (float)DRUMS_FRAMES) {
            polivoks_bank_note(q->pv, e->note, e->vel, 0);
            snd_mark(q, e->note, e->vel != 0);
            e->used = 0;
        } else e->t -= (float)DRUMS_FRAMES;
    }
}
static void notes_off(seq_t *q)
{
    unsigned i;
    for (i = 0; i < 8u; i++) q->ev[i].used = 0;
    for (i = 0; i < 6u; i++) if (q->snd[i] != 255) { polivoks_bank_note(q->pv, q->snd[i], 0, 0); q->snd[i] = 255; }
}

static void fire_drums(seq_t *q, const uint8_t *pat, unsigned n, float at, float sl, float swing_d)
{
    unsigned pad;
    for (pad = 0; pad < DRUMS_NPAD; pad++) {
        unsigned s = seq_get(pat, n, pad), vel, rat = 0, r;
        int tune = 0;
        float nudge = 0.0f;
        seq_hit_t h;
        if (!s) continue;
        vel = s == 2u ? q->acc : q->hit;
        if (seq_hit_get(pat, n, pad, &h)) { vel = h.vel ? h.vel : vel; tune = (int)h.tune - 24; nudge = (float)h.delay * (1.0f / 32.0f) * sl; rat = h.ratchet; }
        for (r = 0; r <= rat; r++) {
            unsigned v = vel - (vel * 15u * r) / 100u;
            drums_note_ex(q->d, drums_note_of_pad(pad), v ? v : 1u, (uint32_t)(at + swing_d + nudge + (float)r * sl / (float)(rat + 1u)), tune);
        }
    }
}
static void fire_plvks(seq_t *q, const uint8_t *pat, unsigned n, float at, float sl, float swing_d)
{
    seq_note_t r;
    float t, gate;
    seq_note_get(pat, n, &r);
    if (!r.on) return;
    t = at + swing_d + (float)r.delay * (1.0f / 32.0f) * sl;
    gate = ((float)r.len + 1.0f) * (1.0f / 16.0f) * sl * 0.97f;
    ev_add(q, t, r.n1, r.amp ? r.amp : 1u);
    ev_add(q, t + gate, r.n1, 0);
    if (r.has2 && r.n2 != r.n1) { ev_add(q, t, r.n2, r.amp ? r.amp : 1u); ev_add(q, t + gate, r.n2, 0); }
}
static void fire(seq_t *q, const uint8_t *pat, float at)   /* play the next step, at samples from the block start */
{
    unsigned len = 16u * ((unsigned)q->len + 1u), n = q->pos == SEQ_NONE ? 0u : (q->pos + 1u) % len;
    float sl = step_samples(q), swing_d = (n & 1u) ? ((float)q->swing * (1.0f / 127.0f)) * 0.5f * sl : 0.0f;   /* swing: the odd steps late */
    if (q->kind == SEQ_KIND_DRUMS) fire_drums(q, pat, n, at, sl, swing_d); else fire_plvks(q, pat, n, at, sl, swing_d);
    q->pos = (uint8_t)n;
}

void seq_init(seq_t *q, drums_bank_t *d, polivoks_bank_t *pv)
{
    unsigned i;
    q->run = 0; q->pos = SEQ_NONE; q->kind = SEQ_KIND_DRUMS;
    q->tempo = 80; q->swing = 0; q->len = 0; q->rate = 1; q->sync = 0; q->hit = 90; q->acc = 127;
    q->next_in = 0.0f; q->est = 0.0f; q->clk = 0; q->since = 0;
    q->d = d; q->pv = pv;
    for (i = 0; i < 8u; i++) q->ev[i].used = 0;
    for (i = 0; i < 6u; i++) q->snd[i] = 255;
}
void seq_kind(seq_t *q, unsigned kind)
{
    seq_command(q, SEQ_STOP);
    q->kind = (uint8_t)(kind ? SEQ_KIND_DRUMS : SEQ_KIND_PLVKS);
}
void seq_set(seq_t *q, unsigned p, unsigned v)
{
    switch (p) {
    case SEQ_TEMPO: q->tempo = (uint8_t)(v > 200u ? 200u : v); break;
    case SEQ_SWING: q->swing = (uint8_t)v; break;
    case SEQ_LEN: q->len = (uint8_t)(v > 3u ? 3u : v); break;
    case SEQ_RATE: q->rate = (uint8_t)(v > 2u ? 2u : v); break;
    case SEQ_SYNC: q->sync = (uint8_t)v; seq_command(q, SEQ_STOP); break;     /* changing the source stops it */
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
        notes_off(q);
    }
}
void seq_clock_in(seq_t *q, const uint8_t *pat)  /* a MIDI clock arrived (24 a quarter note) */
{
    if (!q->run || !q->sync) return;
    if (q->since > 0u) {                                 /* measure the clock: smooth, the block grid is coarse */
        float m = (float)q->since;
        q->est = q->est > 0.0f ? q->est + (m - q->est) * 0.15f : m;
    }
    q->since = 0;
    if (++q->clk >= 24u / steps_per_beat(q)) { q->clk = 0; fire(q, pat, 0.0f); }
}
void seq_block(seq_t *q, const uint8_t *pat)
{
    if (!q->run) return;
    if (q->sync) { if (q->since < 100000u) q->since += DRUMS_FRAMES; }
    else {
        float sl = step_samples(q);
        while (q->next_in < (float)DRUMS_FRAMES) {
            fire(q, pat, q->next_in);
            q->next_in += sl;
        }
        q->next_in -= (float)DRUMS_FRAMES;
    }
    if (q->kind == SEQ_KIND_PLVKS) ev_flush(q);
}
