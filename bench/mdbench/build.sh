#!/bin/bash
# SPDX-License-Identifier: MIT
# bench/mdbench/build.sh [native|aarch64|all] -> out/mdbench-native, out/mdbench-aarch64 (glibc, runs in the DroidDeck guest)
# Env: CC (native cc), CC_A64 (default $MALI_DROIDTEST/tools/cc-aarch64-glibc), NOWL=1 (headless only, no Wayland).
set -euo pipefail
cd "$(dirname "$0")"
V=../../third_party/mesa/include
CF="-std=gnu11 -O2 -g0 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$V -Isrc -Iext"
SRC="src/mdbench.c"
if [ "${NOWL:-0}" != 1 ]; then CF="$CF -DMDB_WAYLAND"; SRC="$SRC ext/xdg-shell-protocol.c"; WL=-lwayland-client; else WL=; fi
mkdir -p out
what=${1:-all}
if [ "$what" = native ] || [ "$what" = all ]; then ${CC:-cc} $CF -o out/mdbench-native $SRC -ldl -lm $WL; echo "built out/mdbench-native"; fi
if [ "$what" = aarch64 ] || [ "$what" = all ]; then
  CC_A64=${CC_A64:-${MALI_DROIDTEST:-$HOME/Projects/mali-gaming/droidtest}/tools/cc-aarch64-glibc}
  "$CC_A64" $CF -o out/mdbench-aarch64 $SRC -ldl -lm $WL; llvm-strip -s out/mdbench-aarch64 2>/dev/null || true; echo "built out/mdbench-aarch64"
fi
