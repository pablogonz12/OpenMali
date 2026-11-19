#!/bin/bash
# SPDX-License-Identifier: MIT
# Unity 2023.2 capability probe (formats + swapchain contract) against the fake-backend ICD. No GPU, no phone.
# Builds t_unity_caps with the host compiler against the system Vulkan loader (it only uses loader entry points, so it works with any
# Mesa tree) and runs it for the GPU profile given as $1 (default v13). Usage: build-t_unity_caps.sh [v10|v11|v13]
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk
OUT=${OUT_DIR:-$V/out/native}; mkdir -p "$OUT"
GPU=${1:-v13}
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
   -o "$OUT/t_unity_caps" "$V/tests/t_unity_caps.c" -lvulkan -lxcb
VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json VK_LOADER_LAYERS_DISABLE='~all~' \
   MALI_BACKEND=fake MALI_FAKE_GPU=$GPU timeout 120 "$OUT/t_unity_caps"
