#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The PLVKS voice and the echo, at the FM-1's own 44.1 kHz, must render bit for bit as they did in another revision
# (default origin/main): tests/core_ref.c prints a hash per scenario, once from that revision's sources and once from
# this tree's.   sh tests/core_check.sh [revision]
set -e
cd "$(dirname "$0")/.."
REF=${1:-origin/main}
git rev-parse --verify -q "$REF^{commit}" >/dev/null || { echo "(no $REF in this checkout: skipped)"; exit 0; }
T=$(mktemp -d)
trap 'git worktree remove --force "$T/ref" >/dev/null 2>&1 || true; rm -rf "$T"' EXIT
git worktree add --detach "$T/ref" "$REF" >/dev/null 2>&1
cp tests/core_ref.c "$T/ref/tests/core_ref.c"
cc -O1 -Wall -Wno-unused-function -o "$T/ref.bin" "$T/ref/tests/core_ref.c"
cc -O1 -Wall -Wno-unused-function -o "$T/new.bin" tests/core_ref.c
"$T/ref.bin" > "$T/ref.txt"
"$T/new.bin" > "$T/new.txt"
if diff "$T/ref.txt" "$T/new.txt" >/dev/null; then
    echo "identical to $REF: $(wc -l < "$T/new.txt") scenarios"
else
    echo "DIFFERENT from $REF:"; diff "$T/ref.txt" "$T/new.txt"; exit 1
fi
