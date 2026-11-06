# CULTBOX PLVKS, as a CLAP plugin

The firmware's Polivoks voice (`firmware/src/polivoks.c`) and its BBD echo (`firmware/src/polivoks_fx.c`), as a CLAP
instrument. The plugin compiles those two files as they are in the firmware; it adds only the host wrapper
(`clap/cultbox_plvks.c`). A fix to the voice is a fix to both.

**Status: early.** It passes its own test host (below) and sanitizers, but has not been loaded into a DAW yet.

## What you get

- One stereo instrument: the voice in the middle, the echo in stereo. 40 automatable parameters: the voice's 32 (by name, with
  the firmware's value names: `Mode LP/BP`, `Wave1 Saw`, `Voices Duo`...), the echo's Time / Feedback / Tone / Mix, Echo Sync and
  Echo Note (the echo follows the host's tempo when Sync is on), Vintage and an output Level in dB.
- Notes, pitch bend (+-2 semitones), CC1 (modulation), CC64 (sustain), CC120/123 (all notes off). Like the instrument, it has no
  velocity: any note plays at one level. Duophonic is `Voices = Duo` (the lowest held key plays oscillator I, the highest oscillator II).
- Any sample rate from 8 kHz to 192 kHz and any block size. State is saved with the project.

## Build

Needs a C11 compiler (GCC or Clang) and CMake. The CLAP headers (MIT) are fetched from GitHub, or pass your own:

    cmake -S plugin -B build/plugin [-DCLAP_INCLUDE_DIR=/path/to/clap/include]
    cmake --build build/plugin

`build/plugin/cultbox-plvks.clap` goes in `~/.clap` (Linux), `~/Library/Audio/Plug-Ins/CLAP` (macOS) or
`%COMMONPROGRAMFILES%\CLAP` (Windows, with MinGW or clang; not MSVC). Only Linux is tried so far.

## Test

    sh plugin/test/run.sh

builds the plugin and loads it in `plugin/test/clap_host_test.c`, a small CLAP host. It checks the parameters and their names,
silence and sound, that the host's block size does not change the sound (blocks of 1 to 4096 frames, bit for bit), that the plugin's
samples are the firmware voice's, tempo sync, state save and load, two instances side by side, other sample rates and reset.
`sh tests/core_check.sh` checks the other half: at 44.1 kHz the voice and the echo still render bit for bit as the firmware did.

## What is different from the instrument

- The voice and the echo work in blocks of 128 frames. Notes, MIDI and parameter changes take effect at the start of the next
  block: up to 128 frames (2.7 ms at 48 kHz) late, never lost.
- The slow wander (drift, vintage) and the noise modulator step once per 16 samples, so at higher sample rates they move a little
  faster than on the FM-1. Pitch, filter, envelope and echo times are in real time at any rate.
- No per-channel saturation / tape stage yet (`fxchain.c` keeps its state in globals), no sequencer, arpeggiator, drums or DUBMARINE.

## Next

DRUMS and DUBMARINE are the same kind of port (rate, block size and the echo's per-instance state are done). The effect chain needs
its globals moved into a struct. VST3 and AU can come from the same source with [clap-wrapper](https://github.com/free-audio/clap-wrapper).

The plugin is GPL-3.0-only, like the rest of CULTBOX: see `LICENSING.md`.
