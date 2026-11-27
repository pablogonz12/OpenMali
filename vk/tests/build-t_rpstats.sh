#!/bin/bash
# Host test of MALI_RP_STATS (mali_rpstats.c): the per-pass line, its attachment numbers, the resolve target, the
# live-BO guard and the line cap. No Mesa, no GPU: the pass record is plain data and the sink is stubbed.
set -e
V=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
cc -std=gnu11 -D_GNU_SOURCE -O1 -w -Wall -Wextra -Wno-unused-parameter -I"$V/src" -o "$OUT/t_rpstats" \
   "$V/tests/t_rpstats.c" "$V/src/mali_rpstats.c"
timeout 60 "$OUT/t_rpstats"
