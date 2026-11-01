/* SPDX-License-Identifier: GPL-3.0-only */
/* Integer-only drawing helpers for the BEAT CULT screens (the main loop has no float unit): sine table, square root,
 * anti-aliased discs and rings on the small canvas (gfx.c). Positions are in quarter pixels. */
static const int16_t UI_SIN[91] = {
    0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563, 2845, 3126, 3406,
    3686, 3964, 4240, 4516, 4790, 5063, 5334, 5604, 5872, 6138, 6402, 6664, 6924,
    7182, 7438, 7692, 7943, 8192, 8438, 8682, 8923, 9162, 9397, 9630, 9860, 10087,
    10311, 10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982, 12176, 12365, 12551, 12733,
    12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044, 14189, 14330, 14466, 14598, 14726,
    14849, 14968, 15082, 15191, 15296, 15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964,
    16026, 16083, 16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374, 16382, 16384,
};
static int32_t ui_sin(int32_t deg)              /* Q14 sine of an angle in degrees */
{
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90) return UI_SIN[deg];
    if (deg <= 180) return UI_SIN[180 - deg];
    if (deg <= 270) return -UI_SIN[deg - 180];
    return -UI_SIN[360 - deg];
}
static int32_t ui_cos(int32_t deg) { return ui_sin(deg + 90); }

static uint32_t ui_isqrt(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

static uint16_t ui_mix(uint16_t fg, uint16_t bg, uint32_t a)      /* a: 0 = bg .. 16 = fg */
{
    uint32_t r = ((uint32_t)(fg >> 11) * a + (uint32_t)(bg >> 11) * (16u - a)) / 16u;
    uint32_t g = ((uint32_t)((fg >> 5) & 63u) * a + (uint32_t)((bg >> 5) & 63u) * (16u - a)) / 16u;
    uint32_t b = ((uint32_t)(fg & 31u) * a + (uint32_t)(bg & 31u) * (16u - a)) / 16u;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void ui_blend(int32_t x, int32_t y, uint16_t c, uint32_t a)
{
    uint16_t bg;
    if (!a || (uint32_t)x >= cv_w || (uint32_t)y >= cv_h) return;
    bg = swap16(cv_px[(uint32_t)y * cv_w + (uint32_t)x]);
    cv_pset(x, y, a >= 16u ? c : ui_mix(c, bg, a));
}

/* a filled disc / a ring (half thickness ht4) around (cx4, cy4), radius r4, all in quarter pixels */
static void ui_disc(int32_t cx4, int32_t cy4, int32_t r4, uint16_t c)
{
    int32_t x, y, x0 = ((cx4 - r4) >> 2) - 1, x1 = ((cx4 + r4) >> 2) + 1, y0 = ((cy4 - r4) >> 2) - 1, y1 = ((cy4 + r4) >> 2) + 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            int32_t dx = x * 4 + 2 - cx4, dy = y * 4 + 2 - cy4, a;
            a = (r4 - (int32_t)ui_isqrt((uint32_t)(dx * dx + dy * dy)) + 2) * 4;
            ui_blend(x, y, c, (uint32_t)(a < 0 ? 0 : a > 16 ? 16 : a));
        }
}
static void ui_ring(int32_t cx4, int32_t cy4, int32_t r4, int32_t ht4, uint16_t c)
{
    int32_t x, y, e = r4 + ht4, x0 = ((cx4 - e) >> 2) - 1, x1 = ((cx4 + e) >> 2) + 1, y0 = ((cy4 - e) >> 2) - 1, y1 = ((cy4 + e) >> 2) + 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            int32_t dx = x * 4 + 2 - cx4, dy = y * 4 + 2 - cy4, d, a;
            d = (int32_t)ui_isqrt((uint32_t)(dx * dx + dy * dy)) - r4;
            a = (ht4 + 2 - (d < 0 ? -d : d)) * 4;
            ui_blend(x, y, c, (uint32_t)(a < 0 ? 0 : a > 16 ? 16 : a));
        }
}
/* a stroke from radius ra to rb (pixels) at an angle (degrees, 0 = up, clockwise), round tip of radius w4 */
static void ui_spoke(int32_t cx4, int32_t cy4, int32_t deg, int32_t ra, int32_t rb, int32_t w4, uint16_t c)
{
    int32_t r, s = ui_sin(deg), co = ui_cos(deg);
    for (r = ra; r <= rb; r++)
        ui_disc(cx4 + ((r * 4 * s) >> 14), cy4 - ((r * 4 * co) >> 14), w4, c);
}
