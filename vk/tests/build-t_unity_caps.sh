#!/bin/bash
# SPDX-License-Identifier: MIT
# Unity 2023.2 capability probe (formats + swapchain contract) against the fake-backend ICD. No GPU, no phone.
# Builds t_unity_caps with the host compiler against the system Vulkan loader (it only uses loader entry points, so it works with any
# Mesa tree) and runs it for the GPU profile given as $1 (default v13). Usage: build-t_unity_caps.sh [v10|v11|v13]
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk
OUT=${OUT_DIR:-$V/out/native}; mkdir -p "$OUT"
GPU=${1:-v13}
# XCB is optional: t_unity_caps.c has a no-XCB branch and only the swapchain surface rows need it, so a host without libxcb headers or
# libs still builds the probe and runs the format rows (same split as vk/build.sh's t_vk_xcb).
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
   -o "$OUT/t_unity_caps" "$V/tests/t_unity_caps.c" -lvulkan -lxcb 2>"$OUT/t_unity_caps.err" \
   || { if grep -qE "xcb/xcb.h: No such file|cannot find -lxcb|unable to find library -lxcb" "$OUT/t_unity_caps.err"; then
           echo "note: t_unity_caps built without XCB (swapchain surface rows skip)"
           cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
              -o "$OUT/t_unity_caps" "$V/tests/t_unity_caps.c" -lvulkan
        else cat "$OUT/t_unity_caps.err"; exit 1; fi; }
VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json VK_LOADER_LAYERS_DISABLE='~all~' \
   MALI_BACKEND=fake MALI_FAKE_GPU=$GPU timeout 120 "$OUT/t_unity_caps"
