#!/bin/sh
# Fence waits must not be sliced: with a fence that completes 350 ms late, the fake backend must never see a wait that
# returns -ETIMEDOUT. Mutation check: MALI_FAKE_WAIT_SLICE_MS=100 makes the fake cap each wait like the old 100 ms
# slicing, and the same run must then log timeouts (so the grep below can fail).
# usage: t_waitslice.sh <t_queue> <libvulkan_mali.so>
set -u
TQ=$1; LIB=$2
run() { env MALI_BACKEND=fake MALI_FAKE_GPU=v13 MALI_FAKE_SLOW_US=350000 MALI_FAKE_WAIT_LOG=1 "$@" "$TQ" --direct "$LIB" --only waitslice 2>&1; }
# the deliberate 250 ms VK_TIMEOUT wait logs exactly the timeouts of that wait; the late fence must add none
good=$(run) || { echo "FAIL waitslice run: $good"; exit 1; }
n=$(printf '%s\n' "$good" | grep -c 'fence_wait timeout')
[ "$n" -eq 0 ] || { echo "FAIL driver sliced the wait ($n backend timeouts): $good"; exit 1; }
mut=$(run MALI_FAKE_WAIT_SLICE_MS=100) || { echo "FAIL mutant run: $mut"; exit 1; }
m=$(printf '%s\n' "$mut" | grep -c 'fence_wait timeout')
[ "$m" -gt "$n" ] || { echo "FAIL mutation not detected (good=$n mutant=$m)"; exit 1; }
echo "ok waitslice (backend timeouts: $n, sliced mutant: $m)"
