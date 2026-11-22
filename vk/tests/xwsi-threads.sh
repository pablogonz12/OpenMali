#!/bin/bash
# SPDX-License-Identifier: MIT
# A blocked vkAcquireNextImageKHR must not stall the other threads of the same device (CR #323 N4).
# t_vk_threads exhausts swapchain A, blocks a thread in acquire(A) for 5 s, then times a surface query, a second
# swapchain and a present on it in the main thread (2 s watchdog). The wsi mode used to be held under a process-wide
# lock across the whole call, so all of those queued behind the wait - on a multi-threaded client (Steam's client +
# web helper) that is a frozen UI. Needs only a working X server: gamescope's own Xwayland.
# Usage: xwsi-threads.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_threads> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without gamescope/Xwayland or t_vk_threads.
set -u
OUT=$1; GPU=${2:-v10}
T=$(cd "$(dirname "$0")" && pwd); V=$(cd "$T/.." && pwd)
[ -x "$OUT/t_vk_threads" ] || { echo "skipped: no t_vk_threads in $OUT"; exit 0; }
for c in gamescope Xwayland xdpyinfo; do command -v $c >/dev/null || { echo "skipped: no $c"; exit 0; }; done
OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cat >"$W/inner.sh" <<INNER
#!/bin/sh
export MALI_BACKEND=fake MALI_FAKE_GPU=$GPU
export VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json
timeout 90 "$OUT/t_vk_threads" --direct "$OUT/libvulkan_mali.so" >"$W/thr.log" 2>&1
echo \$? >"$W/rc"
INNER
chmod +x "$W/inner.sh"
timeout 200 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
rc=$(cat "$W/rc" 2>/dev/null || echo 1)
if [ "$rc" = 77 ]; then echo "skipped: $(grep -m1 skipped "$W/thr.log")"; exit 0; fi
grep -E "^(surface query|second swapchain|acquire on the second|present on the second)" "$W/thr.log"
[ "$rc" = 0 ] || { echo "FAIL: t_vk_threads exit $rc"; tail -8 "$W/thr.log"; exit 1; }
echo "ok    threads ($GPU): a blocked acquire stalled nothing (surface query, swapchain, acquire and present on another chain)"
