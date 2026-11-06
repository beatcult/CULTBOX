/* SPDX-License-Identifier: GPL-3.0-only */
/* CULTBOX in a browser: tests/host.c's fake FM-1, driven by a web page instead of a script. Included by host.c when
 * CULTBOX_WEB is set; web/build.sh compiles the whole firmware to WebAssembly with it. The page (web/index.html) runs
 * the module in an AudioWorklet: the sound clocks the firmware, which the page asks for 128 frames at a time
 * (web_run), and takes the screen (web_fb), the LEDs and what the firmware saved back out of it. */

void *memcpy(void *d, const void *s, unsigned long n) { __builtin_memcpy(d, s, n); return d; }   /* (the compiler calls them; -mbulk-memory makes them one instruction) */
void *memmove(void *d, const void *s, unsigned long n) { __builtin_memmove(d, s, n); return d; }
void *memset(void *d, int c, unsigned long n) { __builtin_memset(d, c, n); return d; }
int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}

#define WEB_EXPORT(n) __attribute__((used, visibility("default"), export_name(n)))
#define WEB_RING 4096u
static int16_t web_ring[2u * WEB_RING];         /* the sound the firmware has made and the page has not yet taken */
static uint32_t web_rw, web_rr;
static float web_out[2][1024];

static void platform_poll(void)                 /* one call = one millisecond, as on the computer */
{
    static uint32_t owed;
    static int32_t blk[2u * HALF_FRAMES];
    fm1_ms++;
    owed += FS;
    while (owed >= 1000u * HALF_FRAMES) {
        uint32_t i;
        owed -= 1000u * HALF_FRAMES;
        memset(blk, 0, sizeof blk);
        app_audio(blk, HALF_FRAMES);
        for (i = 0; i < HALF_FRAMES; i++) {
            uint32_t at = (web_rw++ % WEB_RING) * 2u;
            int32_t l = blk[2u * i], r = blk[2u * i + 1u];
            web_ring[at] = (int16_t)(l > 32767 ? 32767 : l < -32767 ? -32767 : l);
            web_ring[at + 1u] = (int16_t)(r > 32767 ? 32767 : r < -32767 ? -32767 : r);
        }
        if (web_rw - web_rr > WEB_RING) web_rr = web_rw - WEB_RING;   /* (the page fell behind: the oldest goes) */
    }
}

WEB_EXPORT("web_init") void web_init(void) { app_init(); }

/* run the firmware until n (<= 1024) frames of sound are ready; the sound as floats, left then right (n each) */
WEB_EXPORT("web_run") float *web_run(uint32_t n)
{
    uint32_t i;
    if (n > 1024u) n = 1024u;
    while (web_rw - web_rr < n) app_step();
    for (i = 0; i < n; i++) {
        uint32_t at = (web_rr++ % WEB_RING) * 2u;
        web_out[0][i] = (float)web_ring[at] * (1.0f / 32768.0f);
        web_out[1][i] = (float)web_ring[at + 1u] * (1.0f / 32768.0f);
    }
    return &web_out[0][0];
}
WEB_EXPORT("web_out_stride") uint32_t web_out_stride(void) { return 1024u; }

WEB_EXPORT("web_fb") const uint16_t *web_fb(void) { return fb; }                     /* 240 x 240 RGB565 */
WEB_EXPORT("web_fb_ver") uint32_t web_fb_ver(void) { return fb_ver; }                /* counts every change of the screen */
WEB_EXPORT("web_leds") uint32_t web_leds(void) { return (uint32_t)host_leds; }        /* bit n: LED n (0..13 the buttons, 14.. the keys) */
WEB_EXPORT("web_leds_hi") uint32_t web_leds_hi(void) { return (uint32_t)(host_leds >> 32); }
WEB_EXPORT("web_glow") uint32_t web_glow_lo(void) { return (uint32_t)host_glow; }       /* the dim layer ("something here") */
WEB_EXPORT("web_glow_hi") uint32_t web_glow_hi(void) { return (uint32_t)(host_glow >> 32); }
WEB_EXPORT("web_ms") uint32_t web_ms(void) { return fm1_ms; }

WEB_EXPORT("web_key") void web_key(uint32_t k, uint32_t down)    /* key 0..26 (0 = F) */
{
    if (k > 26u) return;
    if (down) fm1_in.notes |= 1u << k; else fm1_in.notes &= ~(1u << k);
}
WEB_EXPORT("web_btn") void web_btn(uint32_t id, uint32_t down)   /* the buttons, as the script's btndown / btnup */
{
    if (id > 13u) return;
    if (down) { host_edges |= 1u << id; fm1_in.buttons |= 1u << id; }
    else if (fm1_in.buttons >> id & 1u) { host_rel |= 1u << id; fm1_in.buttons &= ~(1u << id); }   /* (a release of a button that was not down is nothing) */
}
WEB_EXPORT("web_enc") void web_enc(uint32_t id, int32_t n)       /* encoder id turned n detents (+ = clockwise) */
{
    host_enc[id % HOST_NENC] += n;
}
WEB_EXPORT("web_midi") void web_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    mq[mq_w % 64u][0] = (uint8_t)st; mq[mq_w % 64u][1] = (uint8_t)d1; mq[mq_w % 64u][2] = (uint8_t)d2; mq[mq_w % 64u][3] = 1;
    mq_w++;
}

/* what the firmware saves (its settings, presets and patterns), for the page to keep in the browser: three buffers */
WEB_EXPORT("web_store") uint8_t *web_store(uint32_t i) { return i == 0 ? host_set : i == 1 ? &host_blob[0][0] : &host_slot[0][0]; }
WEB_EXPORT("web_store_size") uint32_t web_store_size(uint32_t i) { return i == 0 ? (uint32_t)sizeof host_set : i == 1 ? (uint32_t)sizeof host_blob : (uint32_t)sizeof host_slot; }
WEB_EXPORT("web_saves") uint32_t web_saves_n(void) { return web_saves; }
