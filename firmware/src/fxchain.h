/* SPDX-License-Identifier: GPL-3.0-only */
/* Per-channel effects: each channel (drums, PLVKS, DUBMARINE) has its own chain, after the voice and before the mixer:
 *   saturation (five models, 2x oversampled) -> tape (saturation, head bump, wow and flutter, loss) -> vintage (dark, a little hiss).
 * The VINTAGE knob also drifts the voice itself (see polivoks_bank_vintage, dubsiren_vintage, drums_vintage).
 * Everything at 0 is bypassed: no cost and the sound as it was. Float DSP; audio interrupt only; mono Q23 in place. */
#ifndef FM1_FXCHAIN_H
#define FM1_FXCHAIN_H
#include <stdint.h>

#define FXC_CHANNELS 3
#define FXC_PARAMS 8
enum { FXC_VINT, FXC_MODEL, FXC_DRIVE, FXC_SMIX, FXC_TDRIVE, FXC_WOW, FXC_AGE, FXC_TMIX };
enum { FXC_TUBE, FXC_DIODE, FXC_FUZZ, FXC_FOLD, FXC_RECT, FXC_MODELS };

void fxc_reset(void);
void fxc_set(unsigned channel, unsigned parameter, unsigned value);
unsigned fxc_active(unsigned channel);                                  /* 0: all of its stages are off */
void fxc_process(unsigned channel, int32_t *buf, unsigned frames);      /* frames <= 128 */
#endif
