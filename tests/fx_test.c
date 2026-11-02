#include <stdlib.h>
/* BBD echo: audible, decays at moderate feedback, bounded at maximum feedback with full-scale input. */
#include <stdio.h>
#include <math.h>
#include "../firmware/src/polivoks_fx.c"
static double run(int blocks, int on_blocks, double amp, double *late, int *bad)
{
    int32_t in[128], l[128], r[128]; double pk = 0; *late = 0; *bad = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 128; ++i) { in[i] = b < on_blocks ? (int32_t)(amp * 8388608.0 * sin(i * 0.3)) : 0; l[i] = r[i] = 0; }
        polivoks_fx_process(0, in, l, r);
        for (int i = 0; i < 128; ++i) {
            double v = fabs(l[i] / 8388608.0); if (v != v || v > 1.01) *bad = 1;
            if (v > pk) pk = v; if (b > blocks - 40 && v > *late) *late = v;
        }
    }
    return pk;
}
int main(void)
{
    double late; int bad, fails = 0;
    polivoks_fx_reset(); polivoks_fx_set(0, FX_DLY_MIX, 100);
    double p = run(300, 2, 0.5, &late, &bad); printf("echo peak %.3f %s\n", p, p > 0.02 && !bad ? "ok" : "FAIL"); fails += !(p > 0.02 && !bad);
    polivoks_fx_reset(); polivoks_fx_set(0, FX_DLY_MIX, 127); polivoks_fx_set(0, FX_DLY_FEED, 127); polivoks_fx_set(0, FX_DLY_TIME, 20);
    run(4000, 4000, 1.0, &late, &bad); printf("max feedback bounded %s\n", bad ? "FAIL" : "ok"); fails += bad;
    polivoks_fx_reset(); polivoks_fx_set(0, FX_DLY_MIX, 127); polivoks_fx_set(0, FX_DLY_FEED, 80);
    run(1500, 3, 0.8, &late, &bad); printf("repeats die away (late %.5f) %s\n", late, late < 0.003 ? "ok" : "FAIL"); fails += !(late < 0.003);
    /* tempo sync: 1/8 at 120 BPM is 0.25 s, and the two echoes are separate */
    {
        int32_t in[128], l[128], r[128]; long first = -1; double peak = 0;
        polivoks_fx_reset(); polivoks_fx_set(0, FX_DLY_MIX, 127); polivoks_fx_set(0, FX_DLY_FEED, 0);
        polivoks_fx_set(1, FX_DLY_MIX, 0);
        polivoks_fx_sync(0, 1, 5); polivoks_fx_tempo(22050.0f);
        for (int b = 0; b < 120; ++b) { for (int i = 0; i < 128; ++i) { in[i] = 0; l[i] = r[i] = 0; } polivoks_fx_process(0, in, l, r); }
        for (int b = 0; b < 200; ++b) {
            for (int i = 0; i < 128; ++i) { in[i] = (b == 0 && i < 8) ? 4000000 : 0; l[i] = r[i] = 0; }
            polivoks_fx_process(0, in, l, r);
            for (int i = 0; i < 128; ++i) { double v = fabs(l[i] / 8388608.0); if (b > 4 && v > peak) { peak = v; first = (long)b * 128 + i; } }
        }
        printf("synced echo 1/8 at 120 BPM lands at %ld samples (11025) %s\n", first, labs(first - 11025) < 200 ? "ok" : "FAIL"); fails += !(labs(first - 11025) < 200);
        for (int b = 0; b < 4; ++b) { for (int i = 0; i < 128; ++i) { in[i] = 3000000; l[i] = r[i] = 0; } polivoks_fx_process(1, in, l, r); }
        { double o = 0; for (int i = 0; i < 128; ++i) o += fabs((double)l[i]); printf("echo 2 with its mix at 0 stays silent %s\n", o == 0 ? "ok" : "FAIL"); fails += o != 0; }
    }
    return fails;
}
