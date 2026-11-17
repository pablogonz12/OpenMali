#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest-side t_vk_leak via scripts/quick/qdrv (needs Download/leak_{libvulkan_mali.so,t_vk_leak}); args from Download/leak_args.txt
D=/storage/emulated/0/Download; T=/tmp/leak; rm -rf $T; mkdir -p $T
cp $D/leak_libvulkan_mali.so $T/libvulkan_mali.so; cp $D/leak_t_vk_leak $T/t_vk_leak; chmod +x $T/t_vk_leak; cp $D/leak_comp.spv $T/comp.comp.spv
LOG=$T/log; ok=1
timeout 200 $T/t_vk_leak --direct $T/libvulkan_mali.so --spv $T/comp.comp.spv $(cat $D/leak_args.txt 2>/dev/null) >$LOG 2>&1 || ok=0
tr '\t' ' ' <$LOG | tail -40 | sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/'
echo "{\"test\":\"summary\",\"ok\":$([ $ok = 1 ] && echo true || echo false)}"
