#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest-side validation via qdrv: t_vk_draw --loop 20, t_vk_compute, t_vk_meta with env from Download/val_env.txt (e.g. "MALI_HF=330").
D=/storage/emulated/0/Download; T=/tmp/val; rm -rf $T; mkdir -p $T/s
cp $D/val_libvulkan_mali.so $T/libvulkan_mali.so
for t in t_vk_draw t_vk_compute t_vk_meta; do cp $D/val_$t $T/$t; chmod +x $T/$t; done
for f in $D/val_*.spv; do b=$(basename $f); cp $f $T/s/${b#val_}; done
cd $T; ok=1
for e in $(cat $D/val_env.txt 2>/dev/null); do export $e; done
LOOP=$(cat $D/val_loop.txt 2>/dev/null || echo 20)
for c in "t_vk_draw --loop $LOOP" "t_vk_compute --loop 5" "t_vk_meta"; do
  set -- $c; n=$1; shift
  sp="--spv-dir $T/s"; [ $n = t_vk_meta ] && sp=""
  timeout 120 $T/$n --direct $T/libvulkan_mali.so $sp "$@" >$T/$n.log 2>&1; rc=$?
  [ $rc = 0 ] || ok=0
  echo "{\"test\":\"$n\",\"rc\":$rc,\"fail_lines\":$(grep -c '"fail"' $T/$n.log),\"pass_lines\":$(grep -c '"pass":true' $T/$n.log)}"
  grep '"fail"' $T/$n.log | head -3 | sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/'
done
echo "{\"test\":\"summary\",\"ok\":$([ $ok = 1 ] && echo true || echo false)}"
