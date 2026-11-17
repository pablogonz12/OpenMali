#!/bin/bash
# SPDX-License-Identifier: MIT
# Host: validation on the Poco. Usage: valrun.sh "<ENV=.. ENV2=..>" [loop]
set -e; export PATH=$PATH:$HOME/Android/Sdk/platform-tools
V=$(cd "$(dirname "$0")/.." && pwd); S=<serial>; DL=/storage/emulated/0/Download
timeout 100 adb -s $S push $V/out/a64-glibc/libvulkan_mali.so $DL/val_libvulkan_mali.so >/dev/null
for t in t_vk_draw t_vk_compute t_vk_meta; do timeout 100 adb -s $S push $V/out/a64-glibc/$t $DL/val_$t >/dev/null; done
for f in $V/shader/*.spv; do timeout 100 adb -s $S push $f $DL/val_$(basename $f) >/dev/null; done
echo "$1" >$HOME/.cache/mali-tmp/val_env.txt; echo "${2:-20}" >$HOME/.cache/mali-tmp/val_loop.txt
adb -s $S push $HOME/.cache/mali-tmp/val_env.txt $DL/val_env.txt >/dev/null; adb -s $S push $HOME/.cache/mali-tmp/val_loop.txt $DL/val_loop.txt >/dev/null
cd ~/Projects/mali-gaming; timeout 280 scripts/quick/qdrv poco $V/tests/valguest.sh --keep 2>&1 | tail -1
cat "$(ls -t runs/quick/*drv-valguest.sh.jsonl | head -1)"
