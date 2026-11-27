#!/bin/bash
# Host test of the implicit-sync decision logic (mali_isync.c): no Mesa, no GPU, no kernel - the backend seam is
# three function pointers, so ENOTTY (5.10) vs a fence-taking kernel, and MALI_IMPLICIT_SYNC=0, are all reachable.
set -e
V=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
cc -std=gnu11 -D_GNU_SOURCE -O1 -w -Wall -Wextra -Wno-unused-parameter -I"$V/src" -o "$OUT/t_isync" \
   "$V/tests/t_isync.c" "$V/src/mali_isync.c" -lpthread
timeout 60 "$OUT/t_isync"
