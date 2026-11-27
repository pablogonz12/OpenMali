#!/bin/bash
# Host test of MALI_SHARED_STATS (mali_sstats.c): the shared_write / shared_read / present line per shared dma-buf,
# and its rate limit. No Mesa, no GPU, no kernel - the backend seam is bo_host_touch/bo_cache and the log sink is
# stubbed, so a clear-colour BO and an untouched one are both reachable.
set -e
V=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
cc -std=gnu11 -D_GNU_SOURCE -O1 -w -Wall -Wextra -Wno-unused-parameter -I"$V/src" -o "$OUT/t_sstats" \
   "$V/tests/t_sstats.c" "$V/src/mali_sstats.c"
timeout 60 "$OUT/t_sstats"
