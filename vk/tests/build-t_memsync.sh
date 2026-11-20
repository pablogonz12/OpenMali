#!/bin/bash
# Host test for the MEM_SYNC policy of the kbase backend (libkbase fake kernel, no Mesa needed).
set -e
V=$(cd "$(dirname "$0")/.." && pwd); KB=$V/../libkbase; OUT=${OUT:-$V/out/native}; mkdir -p "$OUT"
cc -std=gnu11 -D_GNU_SOURCE -O1 -w -I"$V/src" -I"$KB/include" -I"$KB/src" -o "$OUT/t_memsync" "$V/tests/t_memsync.c" "$V/src/mali_backend_kbase.c" \
   "$KB/src/ctx.c" "$KB/src/bo.c" "$KB/src/queue.c" "$KB/src/sync.c" "$KB/src/heap.c" "$KB/src/draw.c" "$KB/src/draw_v13.c" "$KB/src/fb.c" "$KB/src/backend_fake.c" -lpthread
# Twice: arch 11 defaults, then with the arch-11 NOCOH shortcut off (what a v13 device sees). Only the second
# configuration lets the device-local policy show: with NOCOH on, every non-exported BO is already unsyncable.
KB_FAKE_ARCH=11 timeout 120 "$OUT/t_memsync"
KB_FAKE_ARCH=11 MALI_INT_NOCOH=0 timeout 120 "$OUT/t_memsync"
