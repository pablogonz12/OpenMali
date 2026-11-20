#!/bin/bash
# SPDX-License-Identifier: MIT
# Software-rendered frames on the desktop: t_soft_frame renders through the real command buffer / frame record with the CPU
# rasteriser (MALI_SOFT=1) and prints a per-32x32-tile colour map; this compares it with tests/golden/soft-frame.txt.
#   tests/soft-frame.sh [--bless] [--gpu v10|v11|v13]
# The map is identical on every GPU profile (the CPU path never looks at the arch), so one golden covers all three.
# --bless rewrites the golden: review `git diff vk/tests/golden/soft-frame.txt` and commit it with the change.
set -u
V=$(cd "$(dirname "$0")/.." && pwd)
OUT=$V/out/native
G=$V/tests/golden/soft-frame.txt
GPU=${MALI_SOFT_GPU:-v11}
bless=0
for a in "$@"; do
   case $a in
      --bless) bless=1 ;;
      --gpu) shift; GPU=${1:-v11} ;;
      v10|v11|v13) GPU=$a ;;
      *) echo "usage: soft-frame.sh [--bless] [--gpu v10|v11|v13]"; exit 2 ;;
   esac
done
case ${MALI_TARGET:-native} in native-san) OUT=$V/out/native-san ;; native-cov) OUT=$V/out/native-cov ;; esac
if [ ! -x "$OUT/t_soft_frame" ]; then
   echo "skip: no $OUT/t_soft_frame (build with vk/build.sh)"; exit 0
fi
out=$(MALI_BACKEND=fake MALI_SOFT=1 MALI_FAKE_GPU=$GPU timeout 120 "$OUT/t_soft_frame" --spv-dir "$V/shader" 2>&1)
rc=$?
if [ $rc -ne 0 ]; then
   echo "FAIL: t_soft_frame (exit $rc)"
   echo "$out" | tail -25 | sed 's/^/  /'
   exit 1
fi
if [ $bless = 1 ]; then
   printf '%s\n' "$out" > "$G"
   echo "blessed $G ($(grep -c '^case' "$G") cases)"
   exit 0
fi
if [ ! -f "$G" ]; then
   echo "FAIL: no golden $G (run tests/soft-frame.sh --bless and commit it)"; exit 1
fi
if diff -u "$G" <(printf '%s\n' "$out") > /tmp/hostcheck-soft.diff.$$ 2>&1; then
   rm -f /tmp/hostcheck-soft.diff.$$
   echo "ok soft frames ($(grep -c '^case' "$G") cases, $GPU)"
   exit 0
fi
echo "FAIL: soft frame tiles differ from $G:"
head -40 /tmp/hostcheck-soft.diff.$$ | sed 's/^/  /'
rm -f /tmp/hostcheck-soft.diff.$$
echo "  (a golden that is wrong because the driver changed on purpose: tests/soft-frame.sh --bless, review, commit)"
exit 1
