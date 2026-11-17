#!/bin/bash
# SPDX-License-Identifier: MIT
# Offline (no GPU/phone) stream+descriptor check: every v11 test runs against the FAKE kbase backend (src/backend_fake.c), its CS ring and all BOs
# are dumped at CS_QUEUE_KICK, then tools/decode_stream.py applies static rules and (t_triangle) diffs against the PanVK golden. Prints OK or findings.
# Usage: ./check.sh [--mutate idvs38|pixfmt688] [--v13]      Env: GOLDEN=<draw-flat.jsonl>
set -u
cd "$(dirname "$0")"
MUT=""; V13=0
while [ $# -gt 0 ]; do case $1 in --mutate) MUT=$2; shift;; --v13) V13=1;; esac; shift; done
GOLDEN=${GOLDEN:-../../runs/20261115-cmf-libkbase/panvk-trace/draw-flat.jsonl}
OUT=out-fake; mkdir -p $OUT
SRC="src/ctx.c src/bo.c src/queue.c src/sync.c src/fb.c src/heap.c src/draw.c src/backend_fake.c"
TESTS="t_clear t_triangle t_uniform t_depth t_texture"; ARCH=11
if [ $V13 = 1 ]; then TESTS="t_triangle_v13 t_uniform_v13 t_depth_v13 t_texture_v13"; ARCH=13; SRC="$SRC src/draw_v13.c"; fi
build() { # rebuild only when a source is newer than the binary
  local t=$1 b=$OUT/$1
  if [ ! -x $b ] || [ -n "$(find tests/$t.c tests/*.h $SRC include -newer $b -print -quit)" ]; then
    cc -std=gnu11 -O1 -Wall -Wextra -Iinclude -o $b tests/$t.c $SRC -lm 2>$OUT/$t.build.log || { echo "FAIL build $t (see $OUT/$t.build.log)"; return 1; }
  fi
}
one() {
  local t=$1 d=$OUT/$1.jsonl
  build $t || return 1
  rm -f $d
  KB_FAKE_DUMP=$d KB_FAKE_ARCH=$ARCH KB_FAKE_MUTATE=$MUT timeout 10 $OUT/$t >$OUT/$t.log 2>&1   # the test's own readback verdict is meaningless here
  [ -s $d ] || { echo "FAIL $t: no CS kick captured (see $OUT/$t.log)"; return 1; }
  local g=""; [ $t = t_triangle ] && [ $ARCH = 11 ] && g="--golden $GOLDEN --allow golden/allow-v11.txt"
  python3 -I tools/decode_stream.py check $d --arch $ARCH $g | sed "s/^/$t: /"
}
for t in $TESTS; do one $t > $OUT/$t.res & done; wait
fail=0; for t in $TESTS; do cat $OUT/$t.res; grep -q "FAIL" $OUT/$t.res && fail=1; done
[ $fail = 0 ] && echo "OK ($TESTS)" || exit 1
