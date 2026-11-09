# RETCON-1 for the M-VAVE FM-1

A tape-drift synthesizer firmware for the M-VAVE FM-1, built on the CULTBOX / Dinghy platform (update loader, USB rescue,
USB-MIDI, keys, knobs, screen, flash store). No samples: everything is computed.

**Sound:** 4 voices, each two PolyBLEP oscillators (triangle/saw, pulse, folded sine, reed) with detune, a state-variable
filter (LP/BP/HP) and an ADSR; shared tape wow and flutter; tape saturation; LFSR cassette hiss; a modulated tape/BBD delay;
a 16-step sequencer. 44.1 kHz, float DSP in the audio interrupt, no heap.

## Panel
| Control | Does |
|---|---|
| keys | play (4 voices, oldest stolen) |
| SEL / EDIT / LFO / FX / ENV / GLO | pages 1-6: OSC, FILT, TAPE, DLY, ENV, SEQ (SELECT encoder: next page) |
| KNOB 1-4 | the page's four values |
| OCT- / OCT+ | octave (-2..+2) |
| SEQ | step edit: keys 1-16 toggle steps, keys 17-27 set the selected step's pitch, PRESETS transposes, ALGORITHM selects |
| PLAY | sequencer start/stop (MIDI Start/Stop too) |
| HOME | tap: all notes off; hold 1.5 s: starting sound |
| SAVE | store now (also automatic 2 s after a change, when silent) |
| MIDI | notes, CC123, CC74 cutoff, CC71 resonance, CC73 attack, CC72 release, CC91 delay mix |

## Build
```
export JIELI_TOOLCHAIN=~/.jieli/toolchain   # sh tools/get_toolchain.sh
export AC79_SDK=~/fw-AC79_AIoT_SDK          # AC79NN_SDK_V1.2.1 (gitee)
APP=retcon1 ./build.sh                      # -> build/retcon1.fwsc (package identity FM-1_334)
```
Install `build/retcon1.fwsc` with M-VAVE's M-UPGRADE exactly as for CULTBOX; a USB rescue (hold OCT- at power-on)
works as in CULTBOX. The identity `FM-1_334` is a pick that has not been checked against M-UPGRADE; change `FM1_IDENTITY`
in `tools/build.py` if it clashes. GitHub Actions (`.github/workflows/retcon1.yml`) builds it on every push; a tag
`retcon1-vX.Y` attaches it to a release.

## Test without hardware
```
gcc -std=c99 -O2 -Wall -Wextra -DRC_POOL= -o /tmp/engine_test tests/retcon/engine_test.c -lm && /tmp/engine_test
APP=retcon1 sh tests/run_host.sh tests/retcon/play.txt     # build/host/retcon1.wav and screens as PNG
```

## Layout
`firmware/src/retcon1.c` (panel app), `firmware/src/dsp/*.h` (oscillators, filter, envelope, tape, delay, voice),
`firmware/src/engine/*.h` (allocator, sequencer, synth), `tests/retcon/`.

GPL-3.0-only, like CULTBOX. Credits: Dinghy (zvenson), CULTBOX (beatcult), Felucca, SLOOP; the sound is inspired by
AudioWarp's Retcon.
