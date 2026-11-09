/* SPDX-License-Identifier: GPL-3.0-only */
/* RETCON-1 engine configuration (C99, header-only, no libm, no heap).
 *
 * The FM-1 platform runs the audio interrupt at 44.1 kHz (platform.c FS), so the engine is
 * retargeted from the spec's 32 kHz to 44.1 kHz. All float math lives in the audio interrupt only:
 * the main loop has no float unit (see docs/platform/RULES.md), so the panel talks to the engine
 * with integers (events) and the engine converts. */
#ifndef RETCON_CONFIG_H
#define RETCON_CONFIG_H
#include <stddef.h>
#include <stdint.h>

#define RC_SAMPLE_RATE 44100.0f
#define RC_INV_SAMPLE_RATE (1.0f / 44100.0f)
#define RC_POLYPHONY 4

/* Control-rate divider: pitch, filter coefficients, tape drift and the sequencer clock advance
 * once per RC_CTRL_DIV samples (2756 Hz). Audio-rate math stays per sample. */
#define RC_CTRL_DIV 16
#define RC_CTRL_RATE (44100.0f / 16.0f)

/* 0.5 s of delay at 44.1 kHz, int16_t, 44 KB. Lives in the platform's zeroed .pool section. */
#define RC_DELAY_SAMPLES 22050

/* Gain staging: 4 voices * RC_VOICE_GAIN * RC_MASTER_GAIN stays around 1.0 full scale. */
#define RC_VOICE_GAIN 0.5f
#define RC_MASTER_GAIN 0.5f

/* Cassette hiss: LFSR clocks per sample (32 = every sample is a fresh 32-bit word). */
#ifndef RC_HISS_STEPS
#define RC_HISS_STEPS 32
#endif

/* Big static buffers: the platform's .pool (zeroed at boot). Host builds define RC_POOL empty. */
#ifndef RC_POOL
#define RC_POOL __attribute__((section(".pool")))
#endif

#endif
