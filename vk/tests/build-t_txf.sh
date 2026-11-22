#!/bin/bash
# SPDX-License-Identifier: MIT
# t_txf: offline compile of a texelFetch fragment SPIR-V (set 2, binding 128)
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
SYS="-lvulkan -lSPIRV-Tools-opt -lSPIRV-Tools -lz -lzstd -ldrm -lm -pthread"
for f in $LIBS; do [ -f $f ] || { echo "missing $B/$f"; exit 1; }; done
W="-I$D/third_party/mesa/include -Wno-unused-parameter -Wno-unused-function -I$V/src"
# A bare `wait` returns 0 even when a child failed, so a failed compile used to link the previous object and pass:
# drop the objects first, then wait on each pid (the compiles stay parallel).
rm -f "$OUT"/mali_compile.o "$OUT"/mali_descriptor.o "$OUT"/mali_descriptor_lower.o "$OUT"/mali_pipeline_layout.o \
      "$OUT"/mali_meta.o "$OUT"/mali_bc.o "$OUT"/t_txf.o "$OUT"/kb_draw.o
pids=()
for f in mali_compile mali_descriptor mali_descriptor_lower mali_pipeline_layout mali_meta mali_bc; do
  eval "$CC $W -c $V/src/$f.c -o $OUT/$f.o" & pids+=($!)
done
eval "$CC $W -c $V/tests/t_txf.c -o $OUT/t_txf.o" & pids+=($!)
eval "$CC -w -I$D/libkbase/include -c $D/libkbase/src/draw.c -o $OUT/kb_draw.o" & pids+=($!)
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=1; done
[ $rc = 0 ] || { echo "t_txf: a compile failed"; exit 1; }
# shellcheck disable=SC2086
c++ -o "$OUT/t_txf" "$OUT/t_txf.o" "$OUT/mali_compile.o" "$OUT/mali_descriptor.o" "$OUT/mali_descriptor_lower.o" "$OUT/mali_pipeline_layout.o" "$OUT/mali_meta.o" "$OUT/mali_bc.o" "$OUT/kb_draw.o" -Wl,--start-group $LIBS -Wl,--end-group $SYS
# the three shaders at the frame's layout, both Valhall arches the phones build (v10 = X4): every texture instruction must
# carry the combined image sampler's own sampler handle (table 3 index 1), txf included.
for a in 10 11; do for s in blitfs_z.frag.spv txf.frag.spv blitfs_o.frag.spv; do
  echo "t_txf arch $a $s"; "$OUT/t_txf" "$a" "$V/shader/$s" || exit 1
done; done
