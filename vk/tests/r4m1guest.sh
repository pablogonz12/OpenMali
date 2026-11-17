#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest-side R4-M1 check via scripts/quick/qdrv (needs Download/r4m1_{libvulkan_mali.so,t_vk_info}).
D=/storage/emulated/0/Download; T=/tmp/r4m1; rm -rf $T; mkdir -p $T
cp $D/r4m1_libvulkan_mali.so $T/libvulkan_mali.so; cp $D/r4m1_t_vk_info $T/t_vk_info; chmod +x $T/t_vk_info
cat >$T/icd.json <<J
{"file_format_version":"1.0.1","ICD":{"library_path":"$T/libvulkan_mali.so","api_version":"1.3.0"}}
J
LOG=$T/log; :>$LOG; ok=1
echo "== t_vk_info direct" >>$LOG; $T/t_vk_info --direct $T/libvulkan_mali.so --dump >>$LOG 2>&1 || ok=0
export VK_DRIVER_FILES=$T/icd.json VK_ICD_FILENAMES=$T/icd.json VK_LOADER_LAYERS_DISABLE='~all~'
if command -v vulkaninfo >/dev/null 2>&1; then
  echo "== vulkaninfo --summary" >>$LOG; vulkaninfo --summary >>$LOG 2>&1 || ok=0
  vulkaninfo >$T/full.txt 2>&1; grep -A12 "^GPU0" $T/full.txt >>$LOG; cp $T/full.txt $D/r4m1_vulkaninfo_full.txt; echo "full rc=$? lines=$(wc -l <$T/full.txt)" >>$LOG
  grep -i "error\|VUID\|invalid" $T/full.txt | head -5 >>$LOG
else echo "# no vulkaninfo in guest" >>$LOG; ok=0; fi
tr '\t' ' ' <$LOG >$LOG.2; mv $LOG.2 $LOG
sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/' $LOG
echo "{\"test\":\"summary\",\"ok\":$([ $ok = 1 ] && echo true || echo false)}"
