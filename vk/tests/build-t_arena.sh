#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test for MALI_ARENA_ZERO (mali_cs_emit.c Az): the arena blocks the packers fully overwrite are no longer memset.
# Runs the same fake-backend many_draws frame twice and compares the two statistics hw_submit writes to $MALI_ALLOC_STATS
# ("<bytes asked of hw_alloc> <bytes of those memset>"): the allocation total must be identical (only the memset changed)
# and the zeroed total must drop by the per-draw descriptor blocks. MALI_ARENA_ZERO=1 must restore the old total.
# Needs $OUT/t_vk_draw (build.sh, run by check.sh before the build-t_* cases).
set -u
V=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-$V/out/native}
T="$OUT/t_vk_draw"
if [ ! -x "$T" ]; then echo "skip  t_arena (no $T)"; exit 0; fi
DL="--direct $OUT/libvulkan_mali.so --spv-dir $V/shader"
run() { # tag, extra env...
   local tag=$1; shift
   local f="$OUT/t_arena.$tag.txt"
   rm -f "$f"
   env "$@" MALI_ALLOC_STATS="$f" MALI_BACKEND=fake MALI_FAKE_GPU=v11 timeout 120 \
      "$T" $DL --only many_draws >"$OUT/t_arena.$tag.log" 2>&1 || { echo "FAIL  t_arena: $tag run (see $OUT/t_arena.$tag.log)" >&2; exit 1; }
   [ -s "$f" ] || { echo "FAIL  t_arena: no stats file for $tag" >&2; exit 1; }
   cat "$f"
}
read -r anew znew <<<"$(run new)"
read -r aold zold <<<"$(run old MALI_ARENA_ZERO=1)"
echo "  default (dirty arena): alloc $anew, zeroed $znew"
echo "  MALI_ARENA_ZERO=1:     alloc $aold, zeroed $zold"
rc=0
[ "$anew" = "$aold" ] || { echo "FAIL  t_arena: allocation total changed ($anew vs $aold)"; rc=1; }
[ "$znew" -lt "$zold" ] || { echo "FAIL  t_arena: the default still zeroes $znew of $zold bytes (no gain)"; rc=1; }
[ $rc = 0 ] && echo "ok    t_arena: ${zold}->${znew} zeroed bytes on the same ${anew} allocated, MALI_ARENA_ZERO=1 restores"
exit $rc
