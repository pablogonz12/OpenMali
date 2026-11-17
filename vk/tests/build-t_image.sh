#!/bin/bash
# SPDX-License-Identifier: MIT
# M5: builds and runs t_image (format table, image layout, descriptor bytes vs libkbase). Usage: build-t_image.sh [-v]
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk
B=${MESA_BUILD:-$D/third_party/build-vk}; OUT=$V/out/native; mkdir -p "$OUT"
export PATH=$D/third_party/venv/bin:$PATH
cd "$B"
CMDS=$(ninja -t commands src/panfrost/vulkan/libvulkan_panfrost.so)
CC=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS" | sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g')
LIBS="src/panfrost/compiler/libpanfrost_compiler.a src/panfrost/compiler/midgard/libpanfrost_midgard.a
 src/panfrost/compiler/midgard/libpanfrost_midgard_disasm.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
 src/util/libmesa_util.a src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a src/util/libmesa_util_simd.a
 src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a src/panfrost/compiler/bifrost/libpanfrost_bifrost.a
 src/panfrost/compiler/bifrost/libpanfrost_bifrost_disasm.a src/panfrost/compiler/bifrost/valhall/libpanfrost_valhall_disasm.a
 src/panfrost/model/libpanfrost_model_lib.a src/compiler/spirv/libvtn.a src/panfrost/lib/libpanfrost_lib.a
 src/panfrost/lib/libpan-format-v10.a src/panfrost/lib/libpan-format-v11.a src/panfrost/lib/libpan-format-v13.a src/panfrost/lib/libpan-arch-v10.a src/panfrost/lib/libpan-arch-v11.a
 src/panfrost/lib/libpan-arch-v13.a src/panfrost/lib/kmod/libpankmod_lib.a src/panfrost/genxml/libpanfrost_decode.a
 src/panfrost/genxml/libpandecode-arch-v10.a src/panfrost/genxml/libpandecode-arch-v11.a src/panfrost/genxml/libpandecode-arch-v13.a
 src/vulkan/runtime/libvulkan_runtime.a src/vulkan/runtime/libvulkan_instance.a src/vulkan/util/libvulkan_util.a src/vulkan/runtime/libvulkan_lite_runtime.a"
SYS="-lSPIRV-Tools-opt -lSPIRV-Tools -lz -lzstd -ldrm -lm -pthread"
for f in $LIBS; do [ -f $f ] || { echo "missing $B/$f"; exit 1; }; done
W="-Wno-unused-parameter -Wno-unused-function -I$V/src"
for f in mali_bc mali_format mali_image mali_buffer mali_sampler mali_descriptor; do
  eval "$CC $W -c $V/src/$f.c -o $OUT/$f.o" &
done
eval "$CC $W -c $V/tests/t_image.c -o $OUT/t_image.o" &
eval "$CC -w -I$D/libkbase/include -c $D/libkbase/src/draw.c -o $OUT/kb_draw.o" &
wait
# shellcheck disable=SC2086
c++ -o "$OUT/t_image" "$OUT/t_image.o" "$OUT/mali_bc.o" "$OUT/mali_format.o" "$OUT/mali_image.o" "$OUT/mali_buffer.o" "$OUT/mali_sampler.o" \
  "$OUT/mali_descriptor.o" "$OUT/kb_draw.o" -Wl,--start-group $LIBS -Wl,--end-group $SYS
cd "$D/.."
timeout -k 5 120 "$OUT/t_image" "$@"
