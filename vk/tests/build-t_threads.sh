#!/bin/bash
# SPDX-License-Identifier: MIT
# Builds t_threads with TSan and runs it against the TSan driver (build it first: MALI_SAN=thread MALI_TARGET=native-san ./build.sh).
# Usage: build-t_threads.sh [t_threads args]   (TSAN_OPTIONS can be overridden)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk; OUT=$V/out/native-san
MESA=${MESA_SRC:-$D/third_party/mesa}
[ -f "$OUT/libvulkan_mali.so" ] || { echo "build the TSan driver first"; exit 1; }
gcc -std=gnu11 -O1 -g -fsanitize=thread -Wall -Wextra -Werror -Wno-missing-field-initializers -I"$MESA/include" -o "$OUT/t_threads" "$V/tests/t_threads.c" -ldl -pthread
cd "$D/.."
MALI_BACKEND=fake MALI_FAKE_GPU=${MALI_FAKE_GPU:-v13} TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=0 exitcode=66 second_deadlock_stack=1} \
   timeout -k 5 300 "$OUT/t_threads" --direct "$OUT/libvulkan_mali.so" "$@"
