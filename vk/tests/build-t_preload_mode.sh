#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test for the colour loadOp LOAD pre-frame shader mode (mali_render.c mali_preload_mode, mali_render_pack_fbd_pf,
# MALI_PRELOAD_INTERSECT): INTERSECT + Clean Tile Write off only where safe, every mandatory-ALWAYS case keeps the old FBD.
set -e
V=$(cd "$(dirname "$0")/.." && pwd); KB=$V/../libkbase; OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
MESA=${MESA_SRC:-$V/../third_party/mesa}
cc -std=gnu11 -D_GNU_SOURCE -O1 -Wall -Wextra -Werror -I"$MESA/include" -I"$V/src" -I"$KB/include" \
   -o "$OUT/t_preload_mode" "$V/tests/t_preload_mode.c" "$V/src/mali_render.c" "$KB/src/fb.c" "$KB/src/draw.c" -lm -lpthread
timeout 120 "$OUT/t_preload_mode"
