#!/usr/bin/env bash
# Host-only AFBC RT emission test (mali_render.c + libkbase fb.c): plain cc. Usage: build-t_render_afbc.sh
set -euo pipefail
V=$(cd "$(dirname "$0")/.." && pwd); D=$(dirname "$V"); OUT=$V/out/native; mkdir -p "$OUT"
${CC:-cc} -std=gnu11 -Wall -Wextra -O1 -g -I"$V/src" -I"$D/libkbase/include" -I"$D/libkbase/src" "$V/tests/t_render_afbc.c" "$V/src/mali_render.c" \
   "$D/libkbase/src/fb.c" -lm -o "$OUT/t_render_afbc"
timeout -k 5 60 "$OUT/t_render_afbc"
