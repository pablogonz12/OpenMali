#!/bin/bash
# SPDX-License-Identifier: MIT
# M13: builds and runs t_query (ops-frame CS words) and the static decode_stream rules on its ring dump. No GPU. Usage: build-t_query.sh
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk; OUT=$V/out/native; mkdir -p "$OUT"
cc -Wall -Werror -Wno-unused-function -I"$V/src" -I"$D/libkbase/include" -o "$OUT/t_query" "$V/tests/t_query.c" "$V/src/mali_event.c"
timeout -k 5 120 "$OUT/t_query" "$OUT/t_query.jsonl"
DS="python3 -I $D/libkbase/tools/decode_stream.py"
python3 -I "$V/tests/broken_ops.py" "$OUT/t_query.jsonl" "$OUT/t_query_bad.jsonl"; : > "$OUT/empty.jsonl"
for a in 11 13; do
   $DS check "$OUT/t_query.jsonl" --arch $a || { echo "t_query: good stream rejected"; exit 1; }
   if $DS check "$OUT/t_query_bad.jsonl" --arch $a >/dev/null; then echo "t_query: broken stream NOT caught"; exit 1; fi
   if $DS check "$OUT/empty.jsonl" --arch $a >/dev/null; then echo "t_query: empty dump NOT caught"; exit 1; fi
done
echo "t_query: decode_stream passes good, fails broken and empty"
