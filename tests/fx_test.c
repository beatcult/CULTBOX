/* BBD echo: audible, decays at moderate feedback, bounded at maximum feedback with full-scale input. */
#include <stdio.h>
#include <math.h>
#include "../firmware/src/polivoks_fx.c"
static double run(int blocks, int on_blocks, double amp, double *late, int *bad)
{
    int32_t in[128], l[128], r[128]; double pk = 0; *late = 0; *bad = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 128; ++i) { in[i] = b < on_blocks ? (int32_t)(amp * 8388608.0 * sin(i * 0.3)) : 0; l[i] = r[i] = 0; }
        polivoks_fx_process(in, l, r);
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
    polivoks_fx_reset(); polivoks_fx_set(FX_DLY_MIX, 100);
    double p = run(300, 2, 0.5, &late, &bad); printf("echo peak %.3f %s\n", p, p > 0.02 && !bad ? "ok" : "FAIL"); fails += !(p > 0.02 && !bad);
    polivoks_fx_reset(); polivoks_fx_set(FX_DLY_MIX, 127); polivoks_fx_set(FX_DLY_FEED, 127); polivoks_fx_set(FX_DLY_TIME, 20);
    run(4000, 4000, 1.0, &late, &bad); printf("max feedback bounded %s\n", bad ? "FAIL" : "ok"); fails += bad;
    polivoks_fx_reset(); polivoks_fx_set(FX_DLY_MIX, 127); polivoks_fx_set(FX_DLY_FEED, 80);
    run(1500, 3, 0.8, &late, &bad); printf("repeats die away (late %.5f) %s\n", late, late < 0.003 ? "ok" : "FAIL"); fails += !(late < 0.003);
    return fails;
}
