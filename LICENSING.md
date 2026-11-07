# Licensing

CULTBOX is a fork of [Dinghy](https://github.com/zvenson/dinghy) by zvenson, which carries the platform of [Felucca](https://github.com/hugelton/Felucca), [SLOOP](https://github.com/isod89/sloop-fm1), sloopDX and OberVave. Everything below that was in Dinghy keeps its authors and its licence.

CULTBOX is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The SMK-37 board layer: `firmware/hal/smk_input.h`, `fm1_flash_map.h`, the SMK-37 parts of `fm1_cc.h`, `fm1_flash.h`, `fm1_lcd_hw.h`, `lcd.c`, `ota.c`, `bootguard.h`, `firmware/loader/`, `tools/fm1pkg_make.py`, `tools/fm1_install.py`, `tools/build.py`; `firmware/src/smk_wheel.h`; `tests/ota_test.c`, `ldr_test.c`, `install_test.py`, `smk_wheel_test.c`, `smk_pkg_test.py` | Copyright (C) 2026 M. Geisler (genkigenki/makkuro), from [genkigenki/sloop-fm1](https://github.com/genkigenki/sloop-fm1) branch `smk37` (the reference port of the [fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) kit) |
| The TRS MIDI IN `firmware/src/midi_uart.c` | Felucca's, as sloopDX carries it, with the ring read by content from Felucca [Salt] 0.9-salt14 by ChanceTheMaker (found by keremimo) |
| The update loader's screen: `firmware/loader/ldr_screen.c`, `tests/ldr_screen_test.c` | Copyright (C) 2026 zvenson (from OberVave, the Firmware Hub's look); the idea, a loader that draws, its 5 x 7 letters and the order of things from [Skiff](https://github.com/illuminus5/skiff) by illuminus5, GPL-3.0 |
| From OberVave: the storage (blobs, slots), `LCD_IDLE`, the SMK-37 wiring of `platform.c`, `store.h`, the pad event queue in `smk_input.h`, `tests/smk_flash_test.c`, `tests/run_tests.sh`, the CI workflow | Copyright (C) 2026 zvenson |
| Dinghy's example app, its wiring `platform.c`, its host mode `tests/host.c` and `run_host.sh`, the platform docs in `docs/platform/` | Copyright (C) 2026 zvenson |
| CULTBOX itself: `firmware/src/cultbox.c`, `polivoks.c/h`, `polivoks_fx.c/h`, `drums.c/h`, `drumseq.c/h`, `dubsiren.c/h`, `fxchain.c/h`, `arp.c/h`, `scale.h`, `presets.h`, `sirenfmt.h`, `ui_gfx.c`, `tools/make_drums.py`, the added `tests/`, `plugin/` (the CLAP plugin and its test host), the README | Copyright (C) 2026 beatcult |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| CLAP headers (`clap/*.h`), not in this tree: `plugin/CMakeLists.txt` fetches tag 1.2.2 (or takes `CLAP_INCLUDE_DIR`) to build the plugin | MIT | <https://github.com/free-audio/clap> |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Samples and artwork

- `assets/drums/elka/` and the generated `firmware/src/drums_data.h`: drum samples recorded from an Elka Drummer One, converted by `tools/make_drums.py`.
- The BEAT CULT logo (`assets/sleepr.svg`, and its bitmap in `firmware/src/logo.h`, drawn by `ui_gfx.c`) is supplied by beatcult and is artwork, not GPL code. Dinghy's own signet (designburg, all rights reserved) is not part of CULTBOX and has been removed.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE", "FM-1", "SMK-37" and "MKE-P37"
belong to their owners. CULTBOX is not made or endorsed by them (nor by Dinghy's, Felucca's or SLOOP's authors).
