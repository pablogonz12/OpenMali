#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest-side R4 draw check via scripts/quick/qdrv (needs Download/r4m1_{libvulkan_mali.so,t_vk_draw,shader.*.spv}); ONLY=case name.
D=/storage/emulated/0/Download; T=/tmp/r4m1d; rm -rf $T; mkdir -p $T/s
cp $D/r4m1_libvulkan_mali.so $T/libvulkan_mali.so; cp $D/r4m1_t_vk_draw $T/t_vk_draw; chmod +x $T/t_vk_draw
for f in $D/r4m1_*.spv; do b=$(basename $f); cp $f $T/s/${b#r4m1_}; done
cd $T; ok=1; $T/t_vk_draw --direct $T/libvulkan_mali.so --spv-dir $T/s --only "$(cat $D/r4m1_only.txt 2>/dev/null || echo mdi)" >$T/log 2>&1 || ok=0
tr '\t' ' ' <$T/log | sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/'
echo "{\"test\":\"summary\",\"ok\":$([ $ok = 1 ] && echo true || echo false)}"
