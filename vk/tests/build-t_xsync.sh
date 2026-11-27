#!/bin/bash
# Two-process test of the cross-process dma-buf fence (mali_xsync.c): fork, no GPU, no Mesa.
set -e
V=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
cc -std=gnu11 -D_GNU_SOURCE -O1 -w -Wall -Wextra -Wno-unused-parameter -I"$V/src" -o "$OUT/t_xsync" \
   "$V/tests/t_xsync.c" "$V/src/mali_xsync.c" -lpthread
timeout 120 "$OUT/t_xsync"
