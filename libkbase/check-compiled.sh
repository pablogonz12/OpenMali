#!/bin/bash
# SPDX-License-Identifier: MIT
# Offline stream check of t_compiled (fake kbase backend + tools/decode_stream.py static rules), like check.sh but for the R2 compiled-shader test.
# Usage: ./check-compiled.sh   (one kick: compiled_identity; the decoder rules assume a single CS per file)
set -u
cd "$(dirname "$0")"
OUT=out-fake; mkdir -p $OUT
SRC="src/ctx.c src/bo.c src/queue.c src/sync.c src/fb.c src/heap.c src/draw.c src/backend_fake.c"
cc -std=gnu11 -O1 -Wall -Wextra -Iinclude -o $OUT/t_compiled tests/t_compiled.c $SRC -lm 2>$OUT/t_compiled.build.log || { echo "FAIL build (see $OUT/t_compiled.build.log)"; exit 1; }
rm -f $OUT/t_compiled.jsonl
KB_FAKE_DUMP=$OUT/t_compiled.jsonl KB_FAKE_ARCH=11 timeout 20 $OUT/t_compiled --only 2 >$OUT/t_compiled.log 2>&1
[ -s $OUT/t_compiled.jsonl ] || { echo "FAIL t_compiled: no CS kick captured (see $OUT/t_compiled.log)"; exit 1; }
python3 -I tools/decode_stream.py check $OUT/t_compiled.jsonl --arch 11 | sed 's/^/t_compiled: /'
