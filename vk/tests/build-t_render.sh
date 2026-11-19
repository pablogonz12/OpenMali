#!/bin/bash
# SPDX-License-Identifier: MIT
# t_render: mali_render.c (clear words, mali_unorm8, FBD packing) on the host; needs only libkbase (no Mesa). Usage: build-t_render.sh
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk; OUT=$V/out/native; mkdir -p "$OUT"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wno-unused-function -I"$V/src" -I"$D/libkbase/include" -o "$OUT/t_render" \
   "$V/tests/t_render.c" "$V/src/mali_render.c" "$D/libkbase/src/fb.c" "$D/libkbase/src/draw.c" -lm
timeout -k 5 60 "$OUT/t_render" "$@"
