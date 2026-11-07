# CULTBOX engines as CLAP plugins

Two CLAP instruments built from the firmware's own source files, with its BBD echo (`firmware/src/polivoks_fx.c`) behind each:

| Plugin | Engine | Firmware source |
| --- | --- | --- |
| `cultbox-plvks.clap` | **PLVKS**, the Polivoks-style duophonic voice | `polivoks.c` |
| `cultbox-dubmarine.clap` | **DUBMARINE**, the dub siren (after beatcult/dubmarine) | `dubsiren.c` |

The plugins compile those files as they are in the firmware. What the plugins add is the host wrapper: `clap/cultbox_clap.c`, shared,
and one small file per engine (`clap/plvks.c`, `clap/dubmarine.c`). A fix to an engine is a fix to the firmware and the plugin.

**Status: early.** Both pass their own test host (below) and the sanitizers, but have not been loaded into a DAW yet.

## What you get

- One stereo instrument each: the engine in the middle, the echo in stereo. Automatable parameters: the engine's (by name, with the
  firmware's value names), the echo's Time / Feedback / Tone / Mix, Echo Sync and Echo Note (the echo follows the host's tempo when
  Sync is on), Vintage and an output Level in dB. PLVKS has 32 of its own, DUBMARINE 12.
- **PLVKS:** notes, pitch bend (+-2 semitones), CC1 (modulation), CC64 (sustain), CC120/123 (all notes off). Like the instrument, no
  velocity: any note plays at one level. Duophonic is `Voices = Duo` (the lowest held key plays oscillator I, the highest oscillator II).
  The echo starts dry (Mix 0), as on the FM-1.
- **DUBMARINE:** mono; the keys play the pitch from `Tune` (middle C is Tune itself) with a glide, the latest held key wins. `Hold`
  latches the siren on, `Decay` is the release the echo rings out of, `Shape` picks one of eight LFO shapes. Pitch bend +-2 semitones,
  CC1 deepens the LFO, CC120/123 all notes off. Unlike the FM-1's default, the echo starts on (Mix 50): a siren wants it.
- Any sample rate from 8 kHz to 192 kHz and any block size. State is saved with the project. Instances share nothing, so a host may
  render them on different threads at once.

## Build

Needs a C11 compiler (GCC or Clang) and CMake. The CLAP headers (MIT) are fetched from GitHub, or pass your own:

    cmake -S plugin -B build/plugin [-DCLAP_INCLUDE_DIR=/path/to/clap/include]
    cmake --build build/plugin

`build/plugin/cultbox-plvks.clap` and `cultbox-dubmarine.clap` go in `~/.clap` (Linux), `~/Library/Audio/Plug-Ins/CLAP` (macOS) or
`%COMMONPROGRAMFILES%\CLAP` (Windows, with MinGW or clang; not MSVC). Only Linux is tried so far.

## Test

    sh plugin/test/run.sh

builds the plugins and loads each in `plugin/test/clap_host_test.c`, a small CLAP host. It checks the parameters and their names,
silence and sound, that the host's block size does not change the sound (blocks of 1 to 4096 frames, bit for bit), that the plugin's
samples are the firmware engine's, tempo sync, state save and load (a loaded instance sounds like one set by hand), two instances side
by side and on two threads at once, other sample rates and reset. It also runs clean under AddressSanitizer, UndefinedBehaviorSanitizer
and ThreadSanitizer (build the plugin and the host with `-fsanitize=...`).
`sh tests/core_check.sh` checks the other half: at 44.1 kHz the voice and the echo still render bit for bit as the firmware did.

## What is different from the instrument

- The voice and the echo work in blocks of 128 frames. Notes, MIDI and parameter changes take effect at the start of the next
  block: up to 128 frames (2.7 ms at 48 kHz) late, never lost.
- The slow wander (drift, vintage) and the noise modulator step once per 16 samples, so at higher sample rates they move a little
  faster than on the FM-1. Pitch, filter, envelope, glide, LFO and echo times are in real time at any rate.
- The echo's tape wobble runs free from the moment the plugin starts, so the echo is not sample-identical from one playback to the
  next (a bounce will not match the previous one exactly in the echo's repeats). The voices are.
- No per-channel saturation / tape stage yet (`fxchain.c` keeps its state in globals), no sequencer, arpeggiator or drums.

## Next

DRUMS is the same kind of port (the wrapper takes an engine in one small file). The effect chain needs its globals moved into a
struct. VST3 and AU can come from the same source with [clap-wrapper](https://github.com/free-audio/clap-wrapper).

The plugin is GPL-3.0-only, like the rest of CULTBOX: see `LICENSING.md`.
