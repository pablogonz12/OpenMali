#!/bin/bash
# SPDX-License-Identifier: MIT
# Usage: ./build.sh [native|aarch64|all]   (default: all)
# Outputs: out/libkbase-<arch>.a, out/t_submit-native (x86-64: fails at open()), out/t_submit-aarch64
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
CFLAGS="-std=c11 -O2 -g0 -Wall -Wextra -Werror -pedantic -Iinclude"
CC_A64=${CC_A64:-$PWD/../../droidtest/tools/cc-aarch64-glibc}
SRC="src/ctx.c src/bo.c src/queue.c src/sync.c src/fb.c src/heap.c src/draw.c"
what=${1:-all}
if [ "$what" = native ] || [ "$what" = all ]; then
  ${CC:-cc} $CFLAGS -o out/t_submit-native tests/t_submit.c $SRC
  ${CC:-cc} $CFLAGS -o out/t_clear-native tests/t_clear.c $SRC
  ${CC:-cc} $CFLAGS -o out/t_triangle-native tests/t_triangle.c $SRC -lm
  ${CC:-cc} $CFLAGS -o out/t_uniform-native tests/t_uniform.c $SRC -lm
  ${CC:-cc} $CFLAGS -o out/t_depth-native tests/t_depth.c $SRC -lm
  ${CC:-cc} $CFLAGS -o out/t_texture-native tests/t_texture.c $SRC -lm
  echo "built out/t_submit-native out/t_clear-native out/t_triangle-native out/t_uniform-native out/t_depth-native"
fi
if [ "$what" = aarch64 ] || [ "$what" = all ]; then
  "$CC_A64" $CFLAGS -Wl,--as-needed -o out/t_submit-aarch64 tests/t_submit.c $SRC
  "$CC_A64" $CFLAGS -Wl,--as-needed -o out/t_clear-aarch64 tests/t_clear.c $SRC
  "$CC_A64" $CFLAGS -Wl,--as-needed -o out/t_triangle-aarch64 tests/t_triangle.c $SRC -lm
  "$CC_A64" $CFLAGS -Wl,--as-needed -o out/t_uniform-aarch64 tests/t_uniform.c $SRC -lm
  "$CC_A64" $CFLAGS -Wl,--as-needed -o out/t_depth-aarch64 tests/t_depth.c $SRC -lm
  "$CC_A64" $CFLAGS -Wl,--as-needed -o out/t_texture-aarch64 tests/t_texture.c $SRC -lm
  llvm-strip -s out/t_submit-aarch64 out/t_clear-aarch64 out/t_triangle-aarch64 out/t_uniform-aarch64 out/t_depth-aarch64 out/t_texture-aarch64 2>/dev/null || true
  echo "built out/t_submit-aarch64"
fi
