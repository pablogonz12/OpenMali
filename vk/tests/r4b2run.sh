#!/bin/bash
# SPDX-License-Identifier: MIT
# Host: push the a64-glibc build + spv files to the Poco and run one test binary via qdrv. Usage: r4b2run.sh <bin> [test args...]
# (rebuild first: MESA_BUILD=$PWD/third_party/build-vk-a64-glibc MALI_TARGET=a64-glibc vk/build.sh)
set -e; export PATH=$PATH:$HOME/Android/Sdk/platform-tools
V=$(cd "$(dirname "$0")/.." && pwd); S=FABYWWEUVWFEMZR8; DL=/storage/emulated/0/Download; bin=$1
for f in libvulkan_mali.so $bin; do timeout 100 adb -s $S push $V/out/a64-glibc/$f $DL/r4b2_$f >/dev/null; done
for f in $V/shader/*.spv; do timeout 100 adb -s $S push $f $DL/r4b2_$(basename $f) >/dev/null; done
echo "$*" >/tmp/r4b2_args.txt; adb -s $S push /tmp/r4b2_args.txt $DL/r4b2_args.txt >/dev/null
cd ~/Projects/mali-gaming; timeout 200 scripts/quick/qdrv poco $V/tests/r4b2guest.sh --keep 2>&1 | tail -1
cat "$(ls -t runs/quick/*drv-r4b2guest.sh.jsonl | head -1)" | sed 's/^{"line":"//; s/"}$//; s/\\"/"/g'
