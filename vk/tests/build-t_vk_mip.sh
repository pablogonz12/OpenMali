#!/bin/bash
# SPDX-License-Identifier: MIT
# Builds t_vk_mip (mip-level content + textureLod level selection) for the target the driver was built for.
#   tests/build-t_vk_mip.sh                      native x86-64  -> vk/out/native/t_vk_mip
#   MALI_TARGET=a64-glibc tests/build-t_vk_mip.sh  aarch64 glibc -> vk/out/a64-glibc/t_vk_mip (cpush to the phone)
# Same flags as vk/build.sh takes from the Mesa build tree for the other t_vk_* clients; nothing here touches the driver.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk
T=${MALI_TARGET:-native}
case $T in
  a64-glibc) B=${MESA_BUILD:-$D/third_party/build-vk-a64-glibc};;
  *)        B=${MESA_BUILD:-$D/third_party/build-vk};;
esac
OUT=$V/out/$T; mkdir -p "$OUT"
export PATH=$D/third_party/venv/bin:$PATH
MESA=$D/third_party/mesa
[ -f "$B/build.ninja" ] || { echo "no Mesa build tree at $B"; exit 1; }
cd "$B"
CMDS=$(ninja -t commands src/panfrost/vulkan/libvulkan_panfrost.so)
CCLINE=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS")
CC=$(sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g; s#^([^ ]*/)?ccache +##' <<<"$CCLINE")
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter \
   -I"$MESA/include" -o "$OUT/t_vk_mip" "$V/tests/t_vk_mip.c" -ldl -lm
echo "built $OUT/t_vk_mip"
