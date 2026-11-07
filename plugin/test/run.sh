#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the CLAP plugins and run each in plugin/test/clap_host_test.c.   [CLAP_INCLUDE_DIR=/path/to/clap/include] sh plugin/test/run.sh
# Without CLAP_INCLUDE_DIR the CLAP headers (tag 1.2.10) are cloned into build/clap-src.
set -e
cd "$(dirname "$0")/../.."
if [ -z "$CLAP_INCLUDE_DIR" ]; then
    [ -d build/clap-src ] || git clone --depth 1 --branch 1.2.10 https://github.com/free-audio/clap build/clap-src
    CLAP_INCLUDE_DIR=$PWD/build/clap-src/include
fi
cmake -S plugin -B build/plugin -DCLAP_INCLUDE_DIR="$CLAP_INCLUDE_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build build/plugin
${CC:-cc} -O1 -Wall -Wno-unused-function -I"$CLAP_INCLUDE_DIR" -Ifirmware/src -o build/plugin/clap_host_test plugin/test/clap_host_test.c -ldl -lm -lpthread
for p in plvks dubmarine; do
    echo "=== cultbox-$p.clap"
    build/plugin/clap_host_test build/plugin/cultbox-$p.clap
done
