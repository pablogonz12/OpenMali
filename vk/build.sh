#!/bin/bash
# SPDX-License-Identifier: MIT
# Builds the Vulkan front end against a pinned, unmodified Mesa build tree.
#   ./build.sh            native x86-64 -> out/native/{libvulkan_mali.so,mali_icd.json,t_icd}
# Env: MESA_SRC (default ../third_party/mesa), MESA_BUILD (default ../third_party/build-vk).
# Compile and link flags are taken from the Mesa build tree (the PanVK target is used only as the
# source of flags and the library list; no PanVK object or header is used), so the same script
# works for any Mesa build dir, including the aarch64 glibc and bionic cross builds (R3-PLAN.md M2).
#
# One-time Mesa setup (2.5 min on 4 cores): see README.md.
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd)
V=$D/vk
MESA=${MESA_SRC:-$D/third_party/mesa}
B=${MESA_BUILD:-$D/third_party/build-vk}
OUT=$V/out/${MALI_TARGET:-native}
GEN=$OUT/gen
OBJ=$OUT/obj
export PATH=$D/third_party/venv/bin:$PATH
PY=$D/third_party/venv/bin/python
ORACLE=src/panfrost/vulkan/libvulkan_panfrost.so

[ -f "$B/build.ninja" ] || { echo "no Mesa build tree at $B (see README.md)"; exit 1; }
mkdir -p "$GEN" "$OBJ"

# 1. Entry point tables. mali_* = arch-independent, mali_v11_*/mali_v13_* = per-arch device entry
#    points (weak: whatever is not implemented resolves to NULL and falls back to vk_common_*).
"$PY" "$MESA/src/vulkan/util/vk_entrypoints_gen.py" --xml "$MESA/src/vulkan/registry/vk.xml" \
   --proto --weak --out-h "$GEN/mali_entrypoints.h" --out-c "$GEN/mali_entrypoints.c" \
   --prefix mali --device-prefix mali_v11 --device-prefix mali_v13 --beta false

# MALI_EXTRA_LD: extra objects/libs (bionic: -llog -lsync and the libdrm objects of the Mesa wrap).
# 2. Flags from the Mesa tree.
cd "$B"
CMDS=$(ninja -t commands "$ORACLE")
CCLINE=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS")
LDLINE=$(tail -1 <<<"$CMDS")
[ -n "$CCLINE" ] || { echo "cannot find the reference compile line in $B"; exit 1; }
CC=$(sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g' <<<"$CCLINE")
LDSYS=$(tr ' ' '\n' <<<"$LDLINE" | grep -E '^(/.*\.so(\.[0-9.]+)?|-l[A-Za-z0-9_+.-]+|-pthread)$' | tr '\n' ' ')
CXX=$(cut -d' ' -f1 <<<"$LDLINE")

# 3. Compile (cwd = Mesa build dir: its -I paths are relative).
SRCS="$V/src/mali_instance.c $V/src/mali_physical_device.c $V/src/mali_device.c
      $V/src/mali_memory.c $V/src/mali_sync.c $V/src/mali_queue.c
      $V/src/mali_backend_fake.c $V/src/mali_backend_kbase.c
      $V/src/mali_format.c $V/src/mali_image.c $V/src/mali_buffer.c $V/src/mali_sampler.c
      $V/src/mali_descriptor.c $V/src/mali_descriptor_lower.c $V/src/mali_pipeline_layout.c
      $V/src/mali_compile.c $V/src/mali_resources.c
      $V/src/mali_wsi.c $V/src/mali_meta.c $V/src/mali_bc.c $V/src/mali_meta_vk.c $V/src/mali_query.c $V/src/mali_query_core.c
      $V/src/mali_event.c $V/src/mali_event_vk.c $V/src/mali_cmd.c $V/src/mali_render.c $V/src/mali_cs_emit.c $V/src/mali_pipeline.c $GEN/mali_entrypoints.c"
# libkbase (own, MIT, libc only) is compiled in with the same flags as the rest, warnings not fatal.
KB=$D/libkbase
KBSRCS="$KB/src/ctx.c $KB/src/bo.c $KB/src/queue.c $KB/src/sync.c $KB/src/heap.c
        $KB/src/draw.c $KB/src/draw_v13.c $KB/src/fb.c"
OBJS=""
for s in $KBSRCS; do
   o=$OBJ/kb_$(basename "$s" .c).o
   eval "$CC -D_GNU_SOURCE -I$KB/include -I$KB/src -w -o $o -c $s"
   OBJS="$OBJS $o"
done
for s in $SRCS; do
   o=$OBJ/$(basename "$s" .c).o
   eval "$CC -I$GEN -I$V/src -I$KB/include -Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o $o -c $s"
   OBJS="$OBJS $o"
done

# 4. Link. Runtime libs need --whole-archive (weak entry point overloads), as Mesa does it.
WHOLE="src/vulkan/runtime/libvulkan_lite_runtime.a src/vulkan/runtime/libvulkan_runtime.a
       src/vulkan/runtime/libvulkan_instance.a src/vulkan/wsi/libvulkan_wsi.a"
# M5/M6: pan_format tables, panfrost compiler (Valhall) and what they pull in (no PanVK object).
PANLIBS="src/panfrost/compiler/libpanfrost_compiler.a src/panfrost/compiler/midgard/libpanfrost_midgard.a
      src/panfrost/compiler/midgard/libpanfrost_midgard_disasm.a src/panfrost/compiler/bifrost/libpanfrost_bifrost.a
      src/panfrost/compiler/bifrost/libpanfrost_bifrost_disasm.a src/panfrost/compiler/bifrost/valhall/libpanfrost_valhall_disasm.a
      src/panfrost/model/libpanfrost_model_lib.a src/panfrost/lib/libpanfrost_lib.a
      src/panfrost/lib/libpan-format-v11.a src/panfrost/lib/libpan-format-v13.a src/panfrost/lib/libpan-arch-v11.a
      src/panfrost/lib/libpan-arch-v13.a src/panfrost/lib/kmod/libpankmod_lib.a src/panfrost/genxml/libpanfrost_decode.a
      src/panfrost/genxml/libpandecode-arch-v11.a src/panfrost/genxml/libpandecode-arch-v13.a"
LIBS="$PANLIBS src/vulkan/util/libvulkan_util.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
      src/compiler/spirv/libvtn.a src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a src/util/libxmlconfig.a"
for l in src/loader/libloader_wayland_helper.a; do   # M14: only in Mesa builds configured with -Dplatforms=wayland
   [ -f "$l" ] && LIBS="$LIBS $l"
done
for l in src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a; do
   [ -f "$l" ] && LIBS="$LIBS $l"     # x86 only
done
for l in $WHOLE $LIBS; do
   [ -f "$l" ] || { echo "missing $B/$l: build the Mesa targets listed in README.md"; exit 1; }
done
# shellcheck disable=SC2086
$CXX -o "$OUT/libvulkan_mali.so" -shared -fPIC -Wl,--as-needed -Wl,--no-undefined -Wl,-O1 \
   -Wl,-soname,libvulkan_mali.so $OBJS \
   -Wl,--whole-archive -Wl,--start-group $WHOLE -Wl,--no-whole-archive $LIBS \
   -Wl,--version-script "$MESA/src/vulkan/vulkan.sym" -Wl,-Bsymbolic -Wl,--gc-sections \
   -Wl,--build-id=sha1 $LDSYS ${MALI_EXTRA_LD:-} -Wl,--end-group

# 4b. t_resources: M5/M7 end-to-end, linked statically with the driver objects (white-box, reads descriptor memory).
eval "$CC -I$GEN -I$V/src -I$KB/include -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o $OBJ/t_resources.o -c $V/tests/t_resources.c"
# shellcheck disable=SC2086
$CXX -o "$OUT/t_resources" $OBJ/t_resources.o $OBJS \
   -Wl,--whole-archive -Wl,--start-group $WHOLE -Wl,--no-whole-archive $LIBS $LDSYS ${MALI_EXTRA_LD:-} -Wl,--end-group

# 5. Loader manifest + test.
cat > "$OUT/mali_icd.json" <<J
{"file_format_version":"1.0.1","ICD":{"library_path":"$OUT/libvulkan_mali.so","api_version":"1.3.0"}}
J
${CC%% *} -std=c11 -O2 -Wall -Wextra -Werror -I"$MESA/include" -o "$OUT/t_icd" "$V/tests/t_icd.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -I"$MESA/include" -o "$OUT/t_queue" "$V/tests/t_queue.c" -ldl -pthread

${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_draw" "$V/tests/t_vk_draw.c" -ldl -lm -pthread
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_compute" "$V/tests/t_vk_compute.c" -ldl -lm -pthread
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_meta" "$V/tests/t_vk_meta.c" -ldl -lm -pthread
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_info" "$V/tests/t_vk_info.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_dxvk_caps" "$V/tests/t_dxvk_caps.c" -ldl

[ "$OUT/libvulkan_mali.so" -nt "$V/src/mali_cs_emit.c" ] || { echo "FAILED: libvulkan_mali.so older than sources"; exit 1; }
echo "built $OUT/libvulkan_mali.so ($(stat -c %s "$OUT/libvulkan_mali.so") B) mali_icd.json t_icd"
