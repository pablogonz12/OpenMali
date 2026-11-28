#!/bin/bash
# SPDX-License-Identifier: MIT
# The suballocator unit test (tests/t_suballoc.c, mock backend): built and run plain, then with ASan+UBSan. No GPU.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk; OUT=$V/out/native; mkdir -p "$OUT"
FL="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers -I$V/src -I$D/libkbase/include"
# shellcheck disable=SC2086
cc $FL -o "$OUT/t_suballoc" "$V/tests/t_suballoc.c" "$V/src/mali_suballoc.c" -pthread
timeout -k 5 120 "$OUT/t_suballoc"
# shellcheck disable=SC2086
cc $FL -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -o "$OUT/t_suballoc-san" \
   "$V/tests/t_suballoc.c" "$V/src/mali_suballoc.c" -pthread
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 timeout -k 5 120 "$OUT/t_suballoc-san" | sed 's/^/asan: /'
