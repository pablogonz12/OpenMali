#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test for the FBD Bound Min/Max box (mali_render.c mali_fbd_bounds, MALI_BBOX32): the render area rounded outward
# to the real tile size, not 32 px and not the whole attachment (audit-perf.md item 11).
set -e
V=$(cd "$(dirname "$0")/.." && pwd); KB=$V/../libkbase; OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
MESA=${MESA_SRC:-$V/../third_party/mesa}
cc -std=gnu11 -D_GNU_SOURCE -O1 -Wall -Wextra -Werror -I"$MESA/include" -I"$V/src" -I"$KB/include" \
   -o "$OUT/t_bbox" "$V/tests/t_bbox.c" "$V/src/mali_render.c" "$KB/src/fb.c" "$KB/src/draw.c" -lm
timeout 120 "$OUT/t_bbox"
