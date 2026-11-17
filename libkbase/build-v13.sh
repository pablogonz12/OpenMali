#!/bin/bash
# SPDX-License-Identifier: MIT
# Usage: ./build-v13.sh [native|aarch64|all]   Outputs out/t_{triangle,uniform,depth,texture}_v13-{native,aarch64}. Does not touch build.sh outputs.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
CFLAGS="-std=c11 -O2 -g0 -Wall -Wextra -Werror -pedantic -Iinclude"
CC_A64=${CC_A64:-$PWD/../../droidtest/tools/cc-aarch64-glibc}
SRC="src/ctx.c src/bo.c src/queue.c src/sync.c src/fb.c src/heap.c src/draw.c src/draw_v13.c"
what=${1:-all}
if [ "$what" = native ] || [ "$what" = all ]; then
  for t in t_triangle_v13 t_uniform_v13 t_depth_v13 t_texture_v13; do ${CC:-cc} $CFLAGS -o out/$t-native tests/$t.c $SRC -lm; done
  echo "built out/{t_triangle,t_uniform,t_depth,t_texture}_v13-native"
fi
if [ "$what" = aarch64 ] || [ "$what" = all ]; then
  for t in t_triangle_v13 t_uniform_v13 t_depth_v13 t_texture_v13; do
    "$CC_A64" $CFLAGS -Wl,--as-needed -o out/$t-aarch64 tests/$t.c $SRC -lm
    llvm-strip -s out/$t-aarch64 2>/dev/null || true
  done
  echo "built out/{t_triangle,t_uniform,t_depth,t_texture}_v13-aarch64"
fi
