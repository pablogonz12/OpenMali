#!/bin/bash
# Host test for the VkDeviceMemory BO free list of the kbase backend (libkbase fake kernel, no Mesa needed).
set -e
V=$(cd "$(dirname "$0")/.." && pwd); KB=$V/../libkbase; OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
<<<<<<< HEAD
cc -std=gnu11 -D_GNU_SOURCE -O1 -w -I"$V/src" -I"$KB/include" -I"$KB/src" -o "$OUT/t_bopool" "$V/tests/t_bopool.c" "$V/tests/wait_debug_stub.c" "$V/src/mali_backend_kbase.c" \
=======
cc -std=gnu11 -D_GNU_SOURCE -O1 -w -I"$V/src" -I"$KB/include" -I"$KB/src" -o "$OUT/t_bopool" "$V/tests/t_bopool.c" "$V/tests/debug_stubs.c" "$V/src/mali_backend_kbase.c" \
>>>>>>> origin/lane/cmf-d2c
   "$KB/src/ctx.c" "$KB/src/bo.c" "$KB/src/queue.c" "$KB/src/sync.c" "$KB/src/heap.c" "$KB/src/draw.c" "$KB/src/draw_v13.c" "$KB/src/fb.c" "$KB/src/backend_fake.c" -lpthread
KB_FAKE_ARCH=11 timeout 120 "$OUT/t_bopool"
