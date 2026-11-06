/* SPDX-License-Identifier: GPL-3.0-only */
/* A small CLAP host for cultbox-plvks.clap: loads the plugin the way a DAW does and checks what a DAW relies on.
 *   cc -O1 -I<clap>/include -I../../firmware/src -o clap_host_test plugin/test/clap_host_test.c -ldl -lm
 *   ./clap_host_test build/plugin/cultbox-plvks.clap */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include "polivoks.c"      /* the same voice, called directly, to compare the plugin with */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the host ---- */
static const void *host_ext(const clap_host_t *h, const char *id) { (void)h; (void)id; return 0; }
static void host_nop(const clap_host_t *h) { (void)h; }
static const clap_host_t HOST = {CLAP_VERSION_INIT, 0, "clap_host_test", "cultbox", "", "0", host_ext, host_nop, host_nop, host_nop};

typedef union { clap_event_header_t h; clap_event_note_t n; clap_event_param_value_t p; clap_event_midi_t m; } ev_t;
typedef struct { ev_t e[64]; uint32_t n; } evl_t;
static uint32_t ev_size(const clap_input_events_t *l) { return ((evl_t *)l->ctx)->n; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) { return &((evl_t *)l->ctx)->e[i].h; }
static bool ev_push(const clap_output_events_t *l, const clap_event_header_t *h) { (void)l; (void)h; return true; }

/* a script of things that happen at absolute frames */
typedef struct { uint32_t frame; int kind; int a; double b; } act_t;      /* kind 0 note on (a key, b velocity 0..1), 1 note off, 2 param (a id, b value), 3 MIDI (a = 3 bytes) */
enum { A_ON, A_OFF, A_PARAM, A_MIDI };

static void hdr(clap_event_header_t *h, uint32_t size, uint32_t time, uint16_t type)
{
    h->size = size; h->time = time; h->space_id = CLAP_CORE_EVENT_SPACE_ID; h->type = type; h->flags = 0;
}
/* play the script through a plugin in host blocks of the given sizes (cycled); L and R get `total` frames */
static void render(const clap_plugin_t *pl, const act_t *s, int ns, uint32_t total, const uint32_t *sizes, int nsizes, double tempo, float *L, float *R)
{
    uint32_t pos = 0;
    int bi = 0;
    while (pos < total) {
        uint32_t len = sizes[bi++ % nsizes];
        evl_t el = {0};
        float *ch[2];
        clap_audio_buffer_t ab;
        clap_event_transport_t tr;
        clap_input_events_t in = {&el, ev_size, ev_get};
        clap_output_events_t out = {0, ev_push};
        clap_process_t pr;
        if (pos + len > total) len = total - pos;
        for (int i = 0; i < ns; ++i) {
            if (s[i].frame < pos || s[i].frame >= pos + len) continue;
            ev_t *e = &el.e[el.n++];
            memset(e, 0, sizeof *e);
            uint32_t t = s[i].frame - pos;
            if (s[i].kind == A_ON || s[i].kind == A_OFF) {
                hdr(&e->h, sizeof e->n, t, s[i].kind == A_ON ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
                e->n.note_id = -1; e->n.port_index = 0; e->n.channel = 0; e->n.key = (int16_t)s[i].a; e->n.velocity = s[i].b;
            } else if (s[i].kind == A_PARAM) {
                hdr(&e->h, sizeof e->p, t, CLAP_EVENT_PARAM_VALUE);
                e->p.param_id = (clap_id)s[i].a; e->p.note_id = -1; e->p.port_index = -1; e->p.channel = -1; e->p.key = -1; e->p.value = s[i].b;
            } else {
                hdr(&e->h, sizeof e->m, t, CLAP_EVENT_MIDI);
                e->m.data[0] = (uint8_t)s[i].a; e->m.data[1] = (uint8_t)(s[i].a >> 8); e->m.data[2] = (uint8_t)(s[i].a >> 16);
            }
        }
        memset(&tr, 0, sizeof tr);
        hdr(&tr.header, sizeof tr, 0, CLAP_EVENT_TRANSPORT);
        tr.flags = CLAP_TRANSPORT_HAS_TEMPO; tr.tempo = tempo;
        ch[0] = L + pos; ch[1] = R + pos;
        memset(&ab, 0, sizeof ab);
        ab.data32 = ch; ab.channel_count = 2;
        memset(&pr, 0, sizeof pr);
        pr.frames_count = len; pr.transport = &tr; pr.audio_outputs = &ab; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &out;
        pr.steady_time = pos;
        if (pl->process(pl, &pr) == CLAP_PROCESS_ERROR) { printf("process error\n"); exit(2); }
        pos += len;
    }
}

/* an in-memory stream for the state */
typedef struct { unsigned char *d; size_t n, r; } sbuf_t;
static int64_t sbuf_write(const clap_ostream_t *o, const void *b, uint64_t n) { sbuf_t *x = (sbuf_t *)o->ctx; memcpy(x->d + x->n, b, n); x->n += n; return (int64_t)n; }
static int64_t sbuf_read(const clap_istream_t *i, void *b, uint64_t n) { sbuf_t *x = (sbuf_t *)i->ctx; if (x->r + n > x->n) n = x->n - x->r; memcpy(b, x->d + x->r, n); x->r += n; return (int64_t)n; }

static const clap_plugin_factory_t *factory;
static const clap_plugin_t *make(double sr)
{
    const clap_plugin_t *p = factory->create_plugin(factory, &HOST, "com.beatcult.cultbox.plvks");
    if (!p || !p->init(p) || !p->activate(p, sr, 1, 4096) || !p->start_processing(p)) { printf("could not make the plugin\n"); exit(2); }
    return p;
}
static void destroy_plugin(const clap_plugin_t *p) { p->stop_processing(p); p->deactivate(p); p->destroy(p); }
static double rms(const float *x, uint32_t a, uint32_t b) { double e = 0; for (uint32_t i = a; i < b; ++i) e += (double)x[i] * x[i]; return sqrt(e / (b - a)); }
static int finite_all(const float *x, uint32_t n) { for (uint32_t i = 0; i < n; ++i) if (!isfinite(x[i]) || fabsf(x[i]) > 4.0f) return 0; return 1; }

#define N 96000u
static float A[2][N], B[2][N], C[2][N];

int main(int argc, char **argv)
{
    void *lib;
    const clap_plugin_entry_t *entry;
    const clap_plugin_t *p, *q;
    const clap_plugin_params_t *params;
    uint32_t odd[] = {1, 37, 512, 300, 5, 127, 129, 1000, 64}, b128[] = {128}, big[] = {4096};
    if (argc < 2) { printf("usage: clap_host_test cultbox-plvks.clap\n"); return 2; }
    lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 2; }
    entry = (const clap_plugin_entry_t *)dlsym(lib, "clap_entry");
    CHECK(entry && clap_version_is_compatible(entry->clap_version), "clap_entry found, CLAP %u.%u.%u", entry ? entry->clap_version.major : 0, entry ? entry->clap_version.minor : 0, entry ? entry->clap_version.revision : 0);
    if (!entry || !entry->init(argv[1])) return 2;
    factory = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    CHECK(factory && factory->get_plugin_count(factory) == 1, "one plugin in the factory");
    CHECK(!strcmp(factory->get_plugin_descriptor(factory, 0)->id, "com.beatcult.cultbox.plvks"), "descriptor id %s", factory->get_plugin_descriptor(factory, 0)->id);
    CHECK(factory->create_plugin(factory, &HOST, "no.such.plugin") == 0, "an unknown id gives no plugin");

    /* parameters */
    p = make(48000.0);
    params = (const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS);
    {
        uint32_t n = params->count(p), bad = 0, dup = 0;
        char seen[256] = {0}, text[64];
        double v;
        for (uint32_t i = 0; i < n; ++i) {
            clap_param_info_t in;
            if (!params->get_info(p, i, &in) || !in.name[0] || in.min_value > in.default_value || in.default_value > in.max_value) ++bad;
            else { if (in.id < 256 && seen[in.id]++) ++dup; if (!params->get_value(p, in.id, &v) || v != in.default_value) ++bad; }
        }
        CHECK(n == 40 && !bad && !dup, "%u parameters, all with a name, a range and their default", n);
        CHECK(params->value_to_text(p, POLIVOKS_MODE, 1, text, sizeof text) && !strcmp(text, "BP"), "Mode 1 reads \"%s\"", text);
        CHECK(params->text_to_value(p, POLIVOKS_WAVE1, "Pls 30", &v) && v == 3.0, "\"Pls 30\" is wave 3");
        CHECK(params->value_to_text(p, 39, -6.0, text, sizeof text) && !strcmp(text, "-6.0 dB"), "Level reads \"%s\"", text);
    }
    CHECK(p->get_extension(p, CLAP_EXT_AUDIO_PORTS) && p->get_extension(p, CLAP_EXT_NOTE_PORTS) && p->get_extension(p, CLAP_EXT_STATE) && p->get_extension(p, CLAP_EXT_TAIL), "audio ports, note ports, state and tail are offered");
    CHECK(((const clap_plugin_tail_t *)p->get_extension(p, CLAP_EXT_TAIL))->get(p) == 480000u, "tail is ten seconds");

    /* sound: silence, a note, its release */
    {
        act_t s[] = {{0, A_ON, 48, 0.8}, {24000, A_OFF, 48, 0}};
        render(p, 0, 0, 4800, b128, 1, 120, A[0], A[1]);
        CHECK(rms(A[0], 0, 4800) == 0.0 && rms(A[1], 0, 4800) == 0.0, "no note, no sound");
        destroy_plugin(p); p = make(48000.0);
        render(p, s, 2, N, b128, 1, 120, A[0], A[1]);
        CHECK(finite_all(A[0], N) && finite_all(A[1], N), "a note renders finite, in range");
        CHECK(rms(A[0], 8000, 20000) > 0.02 && rms(A[0], 8000, 20000) < 0.7, "the held note is audible (rms %.3f)", rms(A[0], 8000, 20000));
        CHECK(rms(A[0], 80000, N) < 0.002, "and dies away after note off (rms %.5f)", rms(A[0], 80000, N));
        CHECK(!memcmp(A[0], A[1], N * sizeof(float)), "echo off: both channels carry the same voice");
    }

    /* the host's block size must not matter */
    {
        act_t s[] = {{0, A_ON, 43, 1.0}, {128 * 20, A_ON, 62, 1.0}, {128 * 60, A_PARAM, POLIVOKS_CUTOFF, 30}, {128 * 100, A_OFF, 43, 0},
                     {128 * 140, A_PARAM, 32, 80}, {128 * 140, A_PARAM, 35, 100}, {128 * 200, A_OFF, 62, 0}, {128 * 300, A_MIDI, 0xB0 | (1 << 8) | (90 << 16), 0}};
        destroy_plugin(p); p = make(48000.0);
        render(p, s, 8, N, b128, 1, 120, A[0], A[1]);
        destroy_plugin(p); p = make(48000.0);
        render(p, s, 8, N, odd, 9, 120, B[0], B[1]);
        CHECK(!memcmp(A[0], B[0], N * sizeof(float)) && !memcmp(A[1], B[1], N * sizeof(float)), "host blocks of 1, 37, 512, 300, ... frames give the same sound as 128");
        destroy_plugin(p); p = make(48000.0);
        render(p, s, 8, N, big, 1, 120, C[0], C[1]);
        CHECK(!memcmp(A[0], C[0], N * sizeof(float)), "so do blocks of 4096");
        CHECK(rms(A[1], 40000, 70000) > 0.0 && memcmp(A[0], A[1], N * sizeof(float)), "with the echo up, left and right differ");
    }

    /* an event between block boundaries is delayed to the next one, not lost */
    {
        act_t s[] = {{200, A_ON, 60, 1.0}};
        uint32_t small[] = {64};
        destroy_plugin(p); p = make(48000.0);
        render(p, s, 1, 4800, small, 1, 120, A[0], A[1]);
        uint32_t first = 0;
        while (first < 4800 && A[0][first] == 0.0f) ++first;
        CHECK(first >= 256 && first < 270, "a note at frame 200 starts at the next 128-frame boundary (frame %u)", first);
    }

    /* the plugin is the firmware's voice: the same samples as calling it directly at 48 kHz */
    {
        act_t s[] = {{0, A_ON, 52, 0.9}, {128 * 100, A_OFF, 52, 0}};
        uint32_t m = 128 * 300;
        uint32_t st[POLIVOKS_BANK_BYTES / 4];
        polivoks_bank_t *b = (polivoks_bank_t *)st;
        int32_t out[POLIVOKS_FRAMES];
        unsigned diff = 0;
        destroy_plugin(p); p = make(48000.0);
        render(p, s, 2, m, odd, 9, 120, A[0], A[1]);
        polivoks_bank_init(b); polivoks_bank_set_rate(b, 48000.0f);
        polivoks_bank_note(b, 52, (unsigned)lrint(0.9 * 127), 0);
        for (uint32_t k = 0; k < m / 128; ++k) {
            if (k == 100) polivoks_bank_note(b, 52, 0, 0);
            memset(out, 0, sizeof out);
            polivoks_bank_render(b, out);
            for (int i = 0; i < 128; ++i) if (A[0][k * 128 + i] != (float)out[i] * (1.0f / 8388608.0f)) ++diff;
        }
        CHECK(diff == 0, "the plugin's output is the firmware voice's, sample for sample (%u differences)", diff);
    }

    /* echo sync follows the host's tempo */
    {
        act_t s[] = {{0, A_PARAM, 35, 127}, {0, A_PARAM, 36, 1}, {0, A_PARAM, 37, 5}, {0, A_PARAM, 33, 90}, {0, A_ON, 60, 1.0}, {128 * 8, A_OFF, 60, 0}};
        destroy_plugin(p); p = make(48000.0);
        render(p, s, 6, 48000, b128, 1, 60, A[0], A[1]);
        destroy_plugin(p); p = make(48000.0);
        render(p, s, 6, 48000, b128, 1, 120, B[0], B[1]);
        CHECK(memcmp(A[0], B[0], 48000 * sizeof(float)) != 0, "with Echo Sync on, the host tempo changes the echo");
    }

    /* state */
    {
        act_t s[] = {{0, A_PARAM, POLIVOKS_CUTOFF, 33}, {0, A_PARAM, POLIVOKS_WAVE1, 3}, {0, A_PARAM, 35, 99}, {0, A_PARAM, 39, -12.5}, {0, A_PARAM, 36, 1}};
        static unsigned char blob[4096];
        sbuf_t ws = {blob, 0, 0};
        clap_ostream_t os; clap_istream_t is;
        const clap_plugin_state_t *st;
        double v1, v2;
        unsigned same = 0;
        destroy_plugin(p); p = make(44100.0);
        render(p, s, 5, 1024, b128, 1, 120, A[0], A[1]);
        os.ctx = &ws; os.write = sbuf_write; is.ctx = &ws; is.read = sbuf_read;
        st = (const clap_plugin_state_t *)p->get_extension(p, CLAP_EXT_STATE);
        CHECK(st->save(p, &os) && ws.n == 8 + 40 * 4, "state saves %zu bytes", ws.n);
        q = make(44100.0);
        CHECK(((const clap_plugin_state_t *)q->get_extension(q, CLAP_EXT_STATE))->load(q, &is), "and loads into a second instance");
        for (uint32_t i = 0; i < 40; ++i) { ((const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS))->get_value(p, i, &v1); params->get_value(q, i, &v2); same += v1 == v2; }
        CHECK(same == 40, "all 40 parameters come back the same (%u)", same);
        ws.r = 0; ws.d[0] ^= 0xFF;
        CHECK(!((const clap_plugin_state_t *)q->get_extension(q, CLAP_EXT_STATE))->load(q, &is), "a state with the wrong header is refused");
        ws.d[0] ^= 0xFF; ws.n = 20; ws.r = 0;
        CHECK(!((const clap_plugin_state_t *)q->get_extension(q, CLAP_EXT_STATE))->load(q, &is), "so is a truncated one");
        /* the loaded instance sounds like the original */
        {
            act_t play[] = {{0, A_ON, 50, 1.0}};
            ws.n = 8 + 40 * 4; ws.r = 0;
            destroy_plugin(p); p = make(44100.0);
            destroy_plugin(q); q = make(44100.0);
            CHECK(((const clap_plugin_state_t *)q->get_extension(q, CLAP_EXT_STATE))->load(q, &is), "(a fresh instance takes it before and after activation)");
            render(q, play, 1, 20000, b128, 1, 120, B[0], B[1]);
            destroy_plugin(p); p = make(44100.0);
            ws.r = 0; ((const clap_plugin_state_t *)p->get_extension(p, CLAP_EXT_STATE))->load(p, &is);
            render(p, play, 1, 20000, b128, 1, 120, C[0], C[1]);
            CHECK(!memcmp(B[0], C[0], 20000 * sizeof(float)) && rms(B[0], 4000, 20000) > 0.0, "an instance loaded from state sounds the same every time");
        }
        destroy_plugin(q);
    }

    /* two instances in one process share nothing */
    {
        act_t sa[] = {{0, A_ON, 45, 1.0}, {0, A_PARAM, 35, 100}, {0, A_PARAM, 33, 100}, {128 * 50, A_OFF, 45, 0}};
        act_t sb[] = {{0, A_ON, 70, 1.0}, {0, A_PARAM, 35, 127}, {0, A_PARAM, 32, 30}};
        destroy_plugin(p); p = make(48000.0);
        render(p, sa, 4, 48000, b128, 1, 120, A[0], A[1]);
        destroy_plugin(p); p = make(48000.0); q = make(48000.0);
        for (uint32_t pos = 0; pos < 48000; pos += 128) {              /* the two take turns, block by block */
            act_t ta[4], tb[3];
            for (int i = 0; i < 4; ++i) { ta[i] = sa[i]; ta[i].frame = sa[i].frame >= pos && sa[i].frame < pos + 128 ? sa[i].frame - pos : 0xFFFFFFFFu; }
            for (int i = 0; i < 3; ++i) { tb[i] = sb[i]; tb[i].frame = sb[i].frame >= pos && sb[i].frame < pos + 128 ? sb[i].frame - pos : 0xFFFFFFFFu; }
            render(p, ta, 4, 128, b128, 1, 120, B[0] + pos, B[1] + pos);
            render(q, tb, 3, 128, b128, 1, 120, C[0] + pos, C[1] + pos);
        }
        CHECK(!memcmp(A[0], B[0], 48000 * sizeof(float)) && !memcmp(A[1], B[1], 48000 * sizeof(float)) && rms(C[0], 0, 48000) > 0.01,
              "one instance's sound is the same with another playing beside it");
        destroy_plugin(q);
    }

    /* other sample rates and a reset */
    {
        double rates[] = {22050.0, 44100.0, 96000.0, 192000.0};
        for (int i = 0; i < 4; ++i) {
            act_t s[] = {{0, A_ON, 57, 1.0}, {0, A_PARAM, 35, 90}, {0, A_PARAM, 33, 110}};
            uint32_t m = (uint32_t)(rates[i] * 0.5);
            static float big_l[96000], big_r[96000];
            destroy_plugin(p); p = make(rates[i]);
            render(p, s, 3, m > 96000 ? 96000 : m, odd, 9, 120, big_l, big_r);
            CHECK(finite_all(big_l, m > 96000 ? 96000 : m) && rms(big_l, 2000, 12000) > 0.01, "%.0f Hz: renders finite sound", rates[i]);
        }
        destroy_plugin(p); p = make(48000.0);
        {
            act_t s[] = {{0, A_ON, 60, 1.0}, {0, A_PARAM, 35, 127}, {0, A_PARAM, 33, 120}, {0, A_PARAM, 34, 120}, {128 * 20, A_OFF, 60, 0}};
            render(p, s, 5, 48000, b128, 1, 120, A[0], A[1]);
            p->reset(p);
            render(p, 0, 0, 4800, b128, 1, 120, B[0], B[1]);
            CHECK(rms(A[0], 20000, 40000) > 0.0 && rms(B[0], 0, 4800) == 0.0, "reset silences the voice and the echo at once");
        }
    }
    destroy_plugin(p);
    entry->deinit();
    dlclose(lib);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
