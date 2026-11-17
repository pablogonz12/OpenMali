#!/bin/bash
# SPDX-License-Identifier: MIT
# M8: builds and runs t_cmd (command buffers -> frame -> CS, v11 and v13) entirely offline (no adb, no phone):
#   1. t_cmd{11,13}: the ICD (static) + libkbase on its FAKE backend + the M8 sources record the t_uniform scenes through the Vulkan
#      entrypoints; libkbase's backend_fake.c dumps the CS ring + BOs at every kick ($KB_FAKE_DUMP).
#   2. libkbase t_uniform / t_uniform_v13 (the hand-built reference, proven on the phones) are built and dumped the same way.
#   3. tests/cmp_cmd.py diffs both dumps field by field (VAs allowlisted) and runs decode_stream.py's static rules on ours.
# Usage: build-t_cmd.sh [-v]. Whole run is <5 s once the objects are built.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); V=$D/vk; KB=$D/libkbase
MESA=${MESA_SRC:-$D/third_party/mesa}
B=${MESA_BUILD:-$D/third_party/build-vk}; OUT=$V/out/native/tcmd; GEN=$OUT/gen; mkdir -p "$OUT" "$GEN"
export PATH=$D/third_party/venv/bin:$PATH
PY=$D/third_party/venv/bin/python
[ -f "$GEN/mali_entrypoints.c" ] || "$PY" "$MESA/src/vulkan/util/vk_entrypoints_gen.py" --xml "$MESA/src/vulkan/registry/vk.xml" \
   --proto --weak --out-h "$GEN/mali_entrypoints.h" --out-c "$GEN/mali_entrypoints.c" \
   --prefix mali --device-prefix mali_v10 --device-prefix mali_v11 --device-prefix mali_v13 --beta false
cd "$B"
CMDS=$(ninja -t commands src/panfrost/vulkan/libvulkan_panfrost.so)
CCLINE=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS")
LDLINE=$(tail -1 <<<"$CMDS")
CC=$(sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g' <<<"$CCLINE")
LDSYS=$(tr ' ' '\n' <<<"$LDLINE" | grep -E '^(/.*\.so(\.[0-9.]+)?|/usr/.*\.a|-l[A-Za-z0-9_+.-]+|-pthread)$' | tr '\n' ' ')
CXX=$(cut -d' ' -f1 <<<"$LDLINE")

ICD="mali_instance mali_physical_device mali_device mali_memory mali_sync mali_queue mali_backend_fake mali_backend_kbase
 mali_format mali_image mali_buffer mali_sampler mali_descriptor mali_descriptor_lower mali_pipeline_layout mali_compile mali_resources"
M8="mali_cs_emit mali_render mali_cmd mali_pipeline mali_wsi mali_meta mali_bc mali_meta_vk mali_query mali_query_core mali_event mali_event_vk"
W="-Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -I$GEN -I$V/src -I$KB/include"
# Objects are rebuilt when a source/header is newer (the compile line is long; a full rebuild takes ~20 s on 4 cores).
newer() { [ ! -f "$1" ] || [ -n "$(find "$2" "$V"/src/*.h -newer "$1" -print -quit)" ]; }
pids=()
for f in $ICD $M8; do
   if newer "$OUT/$f.o" "$V/src/$f.c"; then eval "$CC $W -o $OUT/$f.o -c $V/src/$f.c" & pids+=($!); fi
done
for f in ctx bo queue sync heap fb draw draw_v13 backend_fake; do
   if newer "$OUT/kb_$f.o" "$KB/src/$f.c"; then eval "$CC -D_GNU_SOURCE -I$KB/include -I$KB/src -w -o $OUT/kb_$f.o -c $KB/src/$f.c" & pids+=($!); fi
done
if newer "$OUT/mali_entrypoints.o" "$GEN/mali_entrypoints.c"; then eval "$CC -D_GNU_SOURCE $W -o $OUT/mali_entrypoints.o -c $GEN/mali_entrypoints.c" & pids+=($!); fi
eval "$CC $W -DARCH=11 -o $OUT/t_cmd11.o -c $V/tests/t_cmd.c" & pids+=($!)
eval "$CC $W -DARCH=13 -o $OUT/t_cmd13.o -c $V/tests/t_cmd.c" & pids+=($!)
for p in "${pids[@]}"; do wait "$p"; done

WHOLE="src/vulkan/runtime/libvulkan_lite_runtime.a src/vulkan/runtime/libvulkan_runtime.a
       src/vulkan/runtime/libvulkan_instance.a src/vulkan/wsi/libvulkan_wsi.a"
PANLIBS="src/panfrost/compiler/libpanfrost_compiler.a src/panfrost/compiler/midgard/libpanfrost_midgard.a
      src/panfrost/compiler/midgard/libpanfrost_midgard_disasm.a src/panfrost/compiler/bifrost/libpanfrost_bifrost.a
      src/panfrost/compiler/bifrost/libpanfrost_bifrost_disasm.a src/panfrost/compiler/bifrost/valhall/libpanfrost_valhall_disasm.a
      src/panfrost/model/libpanfrost_model_lib.a src/panfrost/lib/libpanfrost_lib.a
      src/panfrost/lib/libpan-format-v10.a src/panfrost/lib/libpan-format-v11.a src/panfrost/lib/libpan-format-v13.a src/panfrost/lib/libpan-arch-v10.a src/panfrost/lib/libpan-arch-v11.a
      src/panfrost/lib/libpan-arch-v13.a src/panfrost/lib/kmod/libpankmod_lib.a src/panfrost/genxml/libpanfrost_decode.a
      src/panfrost/genxml/libpandecode-arch-v10.a src/panfrost/genxml/libpandecode-arch-v11.a src/panfrost/genxml/libpandecode-arch-v13.a"
LIBS="$PANLIBS src/vulkan/util/libvulkan_util.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
      src/compiler/spirv/libvtn.a src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a src/util/libxmlconfig.a"
for l in src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a; do [ -f "$l" ] && LIBS="$LIBS $l"; done
OBJS=""; for f in $ICD $M8; do OBJS="$OBJS $OUT/$f.o"; done
for f in ctx bo queue sync heap fb draw draw_v13 backend_fake; do OBJS="$OBJS $OUT/kb_$f.o"; done
for a in 11 13; do
   if [ -x "$OUT/t_cmd$a" ] && [ -z "$(find "$OUT" -maxdepth 1 -name '*.o' -newer "$OUT/t_cmd$a" -print -quit)" ]; then continue; fi
   # shellcheck disable=SC2086
   $CXX -o "$OUT/t_cmd$a" "$OUT/t_cmd$a.o" "$OUT/mali_entrypoints.o" $OBJS -Wl,--start-group -Wl,--whole-archive $WHOLE \
      -Wl,--no-whole-archive $LIBS -Wl,--end-group $LDSYS -lm &
done
wait

# Hand-built libkbase references (same compile as libkbase/check.sh); rebuilt only when a libkbase file is newer.
KSRC="$KB/src/ctx.c $KB/src/bo.c $KB/src/queue.c $KB/src/sync.c $KB/src/fb.c $KB/src/heap.c $KB/src/draw.c $KB/src/backend_fake.c"
stale() { [ ! -x "$1" ] || [ -n "$(find "$KB/src" "$KB/include" "$KB/tests" -type f -newer "$1" -print -quit)" ]; }
if stale "$OUT/ref_uniform11"; then cc -std=gnu11 -O1 -I"$KB/include" -o "$OUT/ref_uniform11" "$KB/tests/t_uniform.c" $KSRC -lm & fi
if stale "$OUT/ref_uniform13"; then cc -std=gnu11 -O1 -I"$KB/include" -o "$OUT/ref_uniform13" "$KB/tests/t_uniform_v13.c" $KSRC "$KB/src/draw_v13.c" -lm & fi
wait
cd "$D/.."
# The 5 s budget covers record + submit + decode + diff (everything after the build).
exec timeout -k 5 120 python3 -I "$V/tests/cmp_cmd.py" "$OUT" "$KB/tools/decode_stream.py" "$(date +%s.%N)" "$@"
