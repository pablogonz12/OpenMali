#!/bin/bash
# SPDX-License-Identifier: MIT
# Builds and runs t_compile against the Mesa build tree (x86-64, offline). Usage: build-t_compile.sh [-v]
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk
B=${MESA_BUILD:-$D/third_party/build-vk}; OUT=$V/out/native; mkdir -p "$OUT"
export PATH=$D/third_party/venv/bin:$PATH
cd "$B"
# Flags: the compile/link lines of a panfrost compiler consumer in the Mesa tree (same trick as compiler-spike/build.sh).
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
eval "$CC -Wno-unused-parameter -Wno-unused-function -I$V/src -c $V/src/mali_compile.c -o $OUT/mali_compile.o"
eval "$CC -Wno-unused-parameter -Wno-unused-function -I$V/src -c $V/src/mali_descriptor_lower.c -o $OUT/mali_descriptor_lower.o"   # M7: mali_compile calls it
eval "$CC -Wno-unused-parameter -Wno-unused-function -I$V/src -c $V/tests/t_compile.c -o $OUT/t_compile.o"
# shellcheck disable=SC2086
c++ -o "$OUT/t_compile" "$OUT/t_compile.o" "$OUT/mali_compile.o" "$OUT/mali_descriptor_lower.o" -Wl,--start-group $LIBS -Wl,--end-group $SYS
cd "$D/.."
timeout -k 5 120 "$OUT/t_compile" "$V/shader" "$@"
