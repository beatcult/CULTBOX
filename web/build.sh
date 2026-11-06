#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# CULTBOX in a browser: the firmware (tests/host.c's fake FM-1 + web/web.c) compiled to WebAssembly with clang,
# no emscripten and no libc.   web/build.sh [outdir]   -> outdir/cultbox.wasm (default build/web)
set -e
cd "$(dirname "$0")/.."
OUT=${1:-build/web}
mkdir -p "$OUT"
clang --target=wasm32 -O2 -ffreestanding -fno-builtin -mbulk-memory -nostdlib -fno-exceptions \
    -Wall -Wno-unused-function -DCULTBOX_HOST -DCULTBOX_WEB -DAPP_VER="\"${APP_VER:-web}\"" \
    -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
    -Wl,--no-entry -Wl,-z,stack-size=262144 -Wl,--lto-O2 -Wl,--gc-sections \
    -o "$OUT/cultbox.wasm" firmware/src/cultbox.c
ls -l "$OUT/cultbox.wasm"
