/* SPDX-License-Identifier: GPL-3.0-only */
/* Sample drums (drums.h). One voice per pad: a hit restarts it. No libm: the pitch ratio comes from a semitone table. */
#include "drums.h"
#include "drums_data.h"

#define KIT DRUM_ELKA
#define HAT_CLOSED 8
#define HAT_OPEN 9
const char *const *const drums_names = DRUM_ELKA_NAMES;
/* General MIDI drum notes of the pads: kick, snare 1, snare 2, tom 1, tom 2, conga, claves, cowbell, hats */
static const uint8_t PAD_NOTE[DRUMS_NPAD] = {36, 38, 40, 50, 45, 63, 75, 56, 42, 46};
static const float SEMI[50] = {0.250000f, 0.264866f, 0.280616f, 0.297302f, 0.314980f, 0.333710f, 0.353553f, 0.374577f, 0.396850f, 0.420448f, 0.445449f, 0.471937f, 0.500000f, 0.529732f, 0.561231f, 0.594604f, 0.629961f, 0.667420f, 0.707107f, 0.749154f, 0.793701f, 0.840896f, 0.890899f, 0.943874f, 1.000000f, 1.059463f, 1.122462f, 1.189207f, 1.259921f, 1.334840f, 1.414214f, 1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f, 2.000000f, 2.118926f, 2.244924f, 2.378414f, 2.519842f, 2.669680f, 2.828427f, 2.996614f, 3.174802f, 3.363586f, 3.563595f, 3.775497f, 4.000000f, 4.237852f};   /* 2^(n/12), n = -24 .. 25 */

typedef struct { float pos, inc, env, envm, gain; uint32_t active, delay; } drum_voice_t;
struct drums_bank {
    drum_voice_t v[DRUMS_NPAD];
    uint8_t pv[DRUMS_PADVALS];
    uint8_t level, crush, tone, choke;
    float lp, held;
    uint32_t hold_n;
};
typedef char drums_bank_fits[sizeof(struct drums_bank) <= DRUMS_BANK_BYTES ? 1 : -1];

int drums_pad_of_note(unsigned note)
{
    unsigned i;
    for (i = 0; i < DRUMS_NPAD; i++) if (PAD_NOTE[i] == note) return (int)i;
    return -1;
}
unsigned drums_note_of_pad(unsigned pad) { return PAD_NOTE[pad % DRUMS_NPAD]; }

void drums_panic(drums_bank_t *b)
{
    unsigned i;
    for (i = 0; i < DRUMS_NPAD; i++) b->v[i].active = 0;
}
void drums_init(drums_bank_t *b)
{
    unsigned i;
    for (i = 0; i < DRUMS_NPAD; i++) {
        b->v[i].pos = b->v[i].inc = b->v[i].env = b->v[i].envm = b->v[i].gain = 0.0f;
        b->v[i].active = 0; b->v[i].delay = 0;
        b->pv[i * 3u + DRUM_TUNE] = 64; b->pv[i * 3u + DRUM_DECAY] = 127; b->pv[i * 3u + DRUM_LEVEL] = 100;
    }
    b->level = 100; b->crush = 0; b->tone = 127; b->choke = 1;
    b->lp = b->held = 0.0f; b->hold_n = 0;
}
void drums_set(drums_bank_t *b, unsigned p, unsigned v)
{
    if (p < DRUMS_PADVALS) b->pv[p] = (uint8_t)v;
    else if (p == DRUMS_LEVEL) b->level = (uint8_t)v;
    else if (p == DRUMS_CRUSH) b->crush = (uint8_t)v;
    else if (p == DRUMS_TONE) b->tone = (uint8_t)v;
    else if (p == DRUMS_CHOKE) b->choke = (uint8_t)v;
}

void drums_note(drums_bank_t *b, unsigned note, unsigned vel) { drums_note_at(b, note, vel, 0); }
void drums_note_at(drums_bank_t *b, unsigned note, unsigned vel, uint32_t delay) { drums_note_ex(b, note, vel, delay, 0); }
void drums_note_ex(drums_bank_t *b, unsigned note, unsigned vel, uint32_t delay, int tune_half)
{
    int pad = drums_pad_of_note(note);
    drum_voice_t *v;
    float st, f, r0, r1;
    int i;
    unsigned tune, decay;
    if (pad < 0 || !vel) return;
    v = &b->v[pad];
    tune = b->pv[pad * 3 + DRUM_TUNE]; decay = b->pv[pad * 3 + DRUM_DECAY];
    st = ((float)tune - 64.0f) * (12.0f / 64.0f) + (float)tune_half * 0.5f;     /* the pad's tune, +-12 semitones, and the step's, +-12 */
    i = (int)st;
    if ((float)i > st) i--;
    f = st - (float)i;
    if (i < -24) { i = -24; f = 0.0f; }
    if (i > 24) { i = 24; f = 0.0f; }
    r0 = SEMI[i + 24]; r1 = SEMI[i + 25];
    v->inc = (float)KIT[pad].rate * (1.0f / 44100.0f) * (r0 + (r1 - r0) * f);
    v->pos = 0.0f;
    v->env = 1.0f;
    if (decay >= 127u) v->envm = 1.0f;                       /* the whole sample */
    else {                                                   /* a fade out, 10 ms .. 0.9 s to -60 dB */
        float x = (float)decay * (1.0f / 127.0f), T = 0.01f + 0.9f * x * x;
        v->envm = 1.0f - 6.9f / (T * 44100.0f);
    }
    v->gain = ((float)vel * (1.0f / 127.0f)) * ((float)b->pv[pad * 3 + DRUM_LEVEL] * 0.01f);
    v->active = 1;
    v->delay = delay;
    if (pad == HAT_CLOSED && b->choke) b->v[HAT_OPEN].active = 0;
}

unsigned drums_render(drums_bank_t *b, int32_t *out)
{
    float mix[DRUMS_FRAMES], master = (float)b->level * 0.01f, a, step;
    unsigned p, i, any = 0, rate_n = 1u + b->crush / 32u;
    for (p = 0; p < DRUMS_NPAD && !b->v[p].active; p++) { }
    if (p == DRUMS_NPAD) return 0;                                   /* nothing sounds: nothing to do */
    for (i = 0; i < DRUMS_FRAMES; i++) mix[i] = 0.0f;
    for (p = 0; p < DRUMS_NPAD; p++) {
        drum_voice_t *v = &b->v[p];
        const int16_t *d = KIT[p].data;
        uint32_t last = KIT[p].len - 1u;
        float pos, inc, env, envm, g;
        if (!v->active) continue;
        pos = v->pos; inc = v->inc; env = v->env; envm = v->envm; g = v->gain;
        i = v->delay < DRUMS_FRAMES ? v->delay : DRUMS_FRAMES;       /* a delayed hit starts part way into the block */
        v->delay -= i;
        for (; i < DRUMS_FRAMES; i++) {
            uint32_t ip = (uint32_t)pos;
            float f, s;
            if (ip >= last || env < 0.0005f) { v->active = 0; break; }
            f = pos - (float)ip;
            s = (float)d[ip] + ((float)d[ip + 1u] - (float)d[ip]) * f;
            mix[i] += s * (g * env);
            env *= envm;
            pos += inc;
        }
        v->pos = pos; v->env = env;
        if (v->active) any = 1;
    }
    a = b->tone >= 127u ? 1.0f : 0.02f + 0.98f * ((float)b->tone * (1.0f / 127.0f)) * ((float)b->tone * (1.0f / 127.0f));
    step = 1.0f + ((float)b->crush * (1.0f / 127.0f)) * ((float)b->crush * (1.0f / 127.0f)) * 4096.0f;
    for (i = 0; i < DRUMS_FRAMES; i++) {
        float y = mix[i] * master;
        int32_t q;
        if (b->crush) {                                      /* fewer bits and a lower rate */
            if (b->hold_n == 0) {
                q = (int32_t)(y / step);
                b->held = (float)q * step;
            }
            b->hold_n = (b->hold_n + 1u) % rate_n;
            y = b->held;
        }
        b->lp += a * (y - b->lp);
        y = b->lp * 256.0f;                                  /* Q15 -> Q23 */
        q = y > 8388607.0f ? 8388607 : y < -8388607.0f ? -8388607 : (int32_t)y;
        out[i] += q;
    }
    return any || b->lp > 1.0f || b->lp < -1.0f;
}
