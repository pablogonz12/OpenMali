#!/bin/bash
# SPDX-License-Identifier: MIT
# Android (bionic) aarch64 test binaries for running directly via `adb shell` from /data/local/tmp.
# Usage: ./build-static.sh   Outputs out-static/<test> (no -aarch64 suffix). Uses the NDK from ~/Android/Sdk.
# Does not touch build.sh / build-v13.sh outputs. Set NDK_CC to override the compiler.
set -euo pipefail
cd "$(dirname "$0")"
NDK=$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/* 2>/dev/null | sort -V | tail -1)
CC=${NDK_CC:-$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang}
mkdir -p out-static
CFLAGS="-std=gnu11 -O2 -g0 -Wall -Wextra -Iinclude"
SRC="src/ctx.c src/bo.c src/queue.c src/sync.c src/fb.c src/heap.c src/draw.c"
for t in t_submit t_clear t_triangle t_uniform t_depth t_texture; do
  "$CC" $CFLAGS -o out-static/$t tests/$t.c $SRC -lm
done
for t in t_triangle_v13 t_uniform_v13 t_depth_v13 t_texture_v13; do
  "$CC" $CFLAGS -o out-static/$t tests/$t.c $SRC src/draw_v13.c -lm
done
"$NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -s out-static/* 2>/dev/null || true
echo "built $(ls out-static | tr '\n' ' ')"
