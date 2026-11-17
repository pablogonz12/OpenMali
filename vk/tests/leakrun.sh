#!/bin/bash
# SPDX-License-Identifier: MIT
# Host: push build + run t_vk_leak on the Poco via qdrv. Usage: leakrun.sh "<t_vk_leak args>"
set -e; export PATH=$PATH:$HOME/Android/Sdk/platform-tools
V=$(cd "$(dirname "$0")/.." && pwd); S=<serial>; DL=/storage/emulated/0/Download
for f in libvulkan_mali.so t_vk_leak; do timeout 100 adb -s $S push $V/out/a64-glibc/$f $DL/leak_$f >/dev/null; done
echo "$1" >$HOME/.cache/mali-tmp/leak_args.txt; adb -s $S push $HOME/.cache/mali-tmp/leak_args.txt $DL/leak_args.txt >/dev/null
cd ~/Projects/mali-gaming; timeout 250 scripts/quick/qdrv poco $V/tests/leakguest.sh --keep 2>&1 | tail -1
cat "$(ls -t runs/quick/*drv-leakguest.sh.jsonl | head -1)"
