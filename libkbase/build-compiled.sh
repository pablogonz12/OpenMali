#!/bin/bash
# SPDX-License-Identifier: MIT
# Bionic aarch64 builds of the R2 tests into out-static/ (for scripts/quick/qdirect), plus glibc builds into out/ for the guest (qdrv / present test).
# Usage: ./build-compiled.sh [static|guest|all]   Tests: t_compiled, t_present_tri (guest only).
set -euo pipefail
cd "$(dirname "$0")"
what=${1:-all}
SRC="src/ctx.c src/bo.c src/queue.c src/sync.c src/fb.c src/heap.c src/draw.c"
CF="-std=gnu11 -O2 -g0 -Wall -Wextra -Iinclude"
TESTS="t_compiled"; [ -f tests/t_present_tri.c ] && TESTS="$TESTS t_present_tri"
if [ $what = static ] || [ $what = all ]; then
  NDK=$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/* | sort -V | tail -1)
  CC=${NDK_CC:-$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang}
  mkdir -p out-static
  for t in $TESTS; do
    [ $t = t_present_tri ] && continue   # guest-only (needs libwayland-client)
    X=""
    "$CC" $CF -o out-static/$t tests/$t.c $SRC $X -lm 2>out-static/$t.build.log || { cat out-static/$t.build.log; exit 1; }
  done
fi
if [ $what = guest ] || [ $what = all ]; then
  CC_A64=${CC_A64:-$PWD/../../droidtest/tools/cc-aarch64-glibc}
  mkdir -p out
  for t in $TESTS; do
    X=""; L=""
    if [ $t = t_present_tri ]; then P=../present; X="$P/gen/xdg-shell-protocol.c $P/gen/linux-dmabuf-v1-protocol.c $P/gen/presentation-time-protocol.c"; L="-lwayland-client"; fi
    "$CC_A64" $CF -I../present/gen -Wno-unused-function -Wl,--as-needed -o out/$t-aarch64 tests/$t.c $SRC $X $L -lm 2>out/$t.build.log || { cat out/$t.build.log; exit 1; }
  done
fi
echo "built $TESTS ($what)"
