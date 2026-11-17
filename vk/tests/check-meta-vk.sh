#!/bin/bash
# SPDX-License-Identifier: MIT
# M12/M13: -Werror compile check of the Vulkan-layer sources (mali_meta_vk.c, mali_query.c, mali_event.c) with the t_cmd flag recipe.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk; KB=$D/libkbase
B=${MESA_BUILD:-$D/third_party/build-vk}; OUT=$V/out/native/meta; GEN=$V/out/native/tcmd/gen; mkdir -p "$OUT"
[ -f "$GEN/mali_entrypoints.h" ] || { echo "run tests/build-t_cmd.sh once first (generates $GEN)"; exit 1; }
export PATH=$D/third_party/venv/bin:$PATH
cd "$B"
CMDS=$(ninja -t commands src/panfrost/vulkan/libvulkan_panfrost.so)
CC=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS" | sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g')
W="-Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -I$GEN -I$V/src -I$KB/include"
for f in "$@"; do eval "$CC $W -o $OUT/$f.o -c $V/src/$f.c"; done
echo "check-meta-vk: ok ($*)"
