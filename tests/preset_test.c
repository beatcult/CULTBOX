/* Every factory preset: sounds, stays finite and under full scale (voice only; the echo is tested apart). */
#include <stdio.h>
#include <math.h>
#include "../firmware/src/polivoks.c"
#include "../firmware/src/presets.h"
static uint32_t st[POLIVOKS_BANK_BYTES / 4]; static int32_t o[128];
int main(void)
{
    int fails = 0;
    for (int i = 0; i < NFACTORY; i++) {
        polivoks_bank_t *b = (polivoks_bank_t *)st; polivoks_bank_init(b);
        for (unsigned p = 0; p < POLIVOKS_NPARAMS; p++) polivoks_bank_set(b, p, FACTORY[i][p]);
        polivoks_bank_note(b, 45, 100, 0); polivoks_bank_note(b, 57, 100, 0);
        double e = 0, pk = 0; long n = 0; int bad = 0;
        for (int k = 0; k < 600; k++) {
            for (int j = 0; j < 128; j++) o[j] = 0;
            polivoks_bank_render(b, o);
            if (k < 20) continue;
            for (int j = 0; j < 128; j++) { double v = o[j] / 8388608.0; if (v != v) bad = 1; e += v * v; n++; if (fabs(v) > pk) pk = fabs(v); }
        }
        double rms = sqrt(e / n);
        int ok = !bad && pk < 0.98 && rms > 0.004;
        printf("%-11s rms %.3f peak %.3f %s\n", FACTORY_NAME[i], rms, pk, ok ? "ok" : "FAIL"); fails += !ok;
    }
    return fails;
}
