/* SPDX-License-Identifier: GPL-3.0-only */
/* Arpeggiator for the PLVKS keys. Runs in the audio interrupt, once a block, on the tempo of the sequencer (samples to a beat):
 * hold keys and they play one after the other, as a rate (1/8, 1/16, 1/16 triplets, 1/32) of the beat, over one to four octaves.
 * Latch keeps the chord after the keys are let go; the next key starts a new one. Notes go out through a callback (velocity 0: off).
 * A second callback hears every note it starts (the recording). */
#ifndef FM1_ARP_H
#define FM1_ARP_H
#include <stdint.h>

#define ARP_MAXN 8
enum { ARP_ON, ARP_MODE, ARP_RATE, ARP_OCTS, ARP_GATE, ARP_NPARAMS };   /* ARP_ON 0 off, 1 on, 2 latch */
enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_ORDER, ARP_RANDOM };
typedef void (*arp_out_t)(void *ctx, unsigned note, unsigned vel);
typedef struct {
    uint8_t on, mode, rate, octs, gate;
    uint8_t held[ARP_MAXN], nheld, vel;                       /* the chord, in the order it was played */
    uint32_t down[4];                                         /* keys physically down (a bit a note) */
    uint8_t list[ARP_MAXN * 4], nl, idx, dir, sound;          /* the arpeggio, where it is, the note sounding (255: none) */
    float next_in, off_in;
    uint32_t rng;
    arp_out_t out, rec; void *ctx;
} arp_t;
void arp_init(arp_t *a, arp_out_t out, arp_out_t rec, void *ctx);
void arp_set(arp_t *a, unsigned parameter, unsigned value);
int arp_note(arp_t *a, unsigned note, unsigned vel);          /* a key; 1 when the arpeggiator took it (it is on) */
void arp_block(arp_t *a, float samples_per_beat);             /* 128 samples pass */
void arp_restart(arp_t *a);                                   /* the next note on the beat, now (the sequencer started) */
void arp_stop(arp_t *a);                                      /* let go of everything */
#endif
