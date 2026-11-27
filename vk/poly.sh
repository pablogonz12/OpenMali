#!/bin/bash
# SPDX-License-Identifier: MIT
# poly.sh — Mesa's src/poly NIR lowering (geometry + tessellation) as a library, for the target build.sh is building.
#   ./poly.sh [target]           default $MALI_TARGET (native), writes out/<target>/poly/libpoly_nir.a
#   ./poly.sh --one-time [t]     also (re)creates the host Mesa configure that generates poly's CL half
#
# What is Mesa's and what is ours (notes/gs-design.md §5(i), §"Lane 2 step i"):
#   * Mesa's: every line of src/poly/nir/*.c, src/poly/{geometry.h,prim.h,tessellator.h,tessellator.h} and the CL half —
#     src/poly/cl/{geometry.cl,tessellation.cl} are compiled to NIR-building C++ (libpoly.cpp/libpoly.h) by Mesa's own
#     meson custom_target (mesa_clc + vtn_bindgen2), which is why a host configure is needed once. No source is patched.
#   * ours: the flags. The archive is compiled from that unmodified source with the *target's* driver flags (the same
#     reference compile line build.sh reads out of the target's Mesa build dir), so poly's NIR code agrees with the
#     libnir.a it is linked against on every target, and the aarch64 ICDs need no cross-configured poly (the generated
#     headers are architecture-independent).
# One host configure also builds the library itself (ninja src/poly/nir/liblibpoly_nir.a) — that is the number quoted for
# "build it once": 42.7 s wall / 1.3 GB RSS on 4 threads from an empty dir, configure 5.5 s.
set -euo pipefail
V=$(cd "$(dirname "$0")" && pwd); D=$(dirname "$V")
MESA=${MESA_SRC:-$D/third_party/mesa}
TARGET=${MALI_TARGET:-native}
B=${MESA_BUILD:-$D/third_party/build-vk}
POLY_BUILD=${POLY_BUILD:-$D/third_party/build-poly}
one_time=0
[ "${1:-}" = --one-time ] && { one_time=1; shift; }
[ $# -gt 0 ] && TARGET=$1
case $TARGET in
   native|native-san|native-cov) [ -z "${MESA_BUILD:-}" ] && [ -f "$D/third_party/build-vk-x86-x11/build.ninja" ] && B=$D/third_party/build-vk-x86-x11 ;;
esac
OUT=$V/out/$TARGET/poly
OBJ=$OUT/obj
export PATH=$D/third_party/venv/bin:$PATH

[ -f "$B/build.ninja" ] || { echo "poly.sh: no Mesa build tree at $B (that target is not configured)"; exit 1; }

# 1. The host configure that owns the generated CL half. Its -Dtools=asahi is the smallest switch that turns with_poly on
#    (meson.build:1046: with_poly = gallium-asahi | asahi-vk | tools=asahi | kosmickrisp-vk); nothing of the AgX driver is
#    requested here — the only targets we ask ninja for are the poly generated headers and libpoly_nir.a itself.
if [ $one_time = 1 ] || [ ! -f "$POLY_BUILD/src/poly/cl/libpoly.h" ]; then
   t0=$SECONDS
   if [ ! -f "$POLY_BUILD/build.ninja" ]; then
      meson setup "$POLY_BUILD" "$MESA" -Dbuildtype=release -Db_ndebug=false \
         -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
         -Dgallium-drivers= -Dvulkan-drivers= -Dtools=asahi -Dllvm=enabled -Dmesa-clc=enabled \
         -Dvalgrind=disabled -Dlibunwind=disabled >"$POLY_BUILD.setup.log" 2>&1 \
         || { echo "poly.sh: meson setup $POLY_BUILD failed (see $POLY_BUILD.setup.log)"; exit 1; }
   fi
   # 402 edges: libnir.a, libvtn.a, mesa_clc, vtn_bindgen2, libpoly.spv -> libpoly.{cpp,h}, then the 6 archive members.
   ninja -C "$POLY_BUILD" src/poly/nir/liblibpoly_nir.a >"$POLY_BUILD.build.log" 2>&1 \
      || { echo "poly.sh: ninja -C $POLY_BUILD src/poly/nir/liblibpoly_nir.a failed (see $POLY_BUILD.build.log)"; exit 1; }
   echo "poly.sh: host configure built ($((SECONDS - t0)) s): $(stat -c %s "$POLY_BUILD/src/poly/nir/liblibpoly_nir.a") B thin archive, $(wc -l <"$POLY_BUILD/src/poly/cl/libpoly.cpp") lines libpoly.cpp"
fi

# 2. The target's flags, exactly as build.sh takes them (one reference compile line + one link line).
CMDS=$(cd "$B" && ninja -t commands src/panfrost/vulkan/libvulkan_panfrost.so)
CCLINE=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS")
LDLINE=$(tail -1 <<<"$CMDS")
[ -n "$CCLINE" ] || { echo "poly.sh: cannot find the reference compile line in $B"; exit 1; }
CC=$(sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g' <<<"$CCLINE")
CC=$(sed -E 's#^([^ ]*/)?ccache +##' <<<"$CC")
CXX=$(cut -d' ' -f1 <<<"$LDLINE"); CXX=$(sed -E 's#^([^ ]*/)?ccache +##' <<<"$CXX")
# poly's own include set (meson src/poly/meson.build: inc_poly = '.', 'nir'; the generated CL half is the build dir's src/poly/cl)
CFLAGS="$CC -I$MESA/src -I$POLY_BUILD/src -Wno-override-init -Wno-c2x-extensions -Wno-unused-parameter ${POLY_EXTRA_CFLAGS:-}"
# The one C++ member (Mesa's generated libpoly.cpp) takes the same flags minus the C-only ones, which cc1plus would warn about.
CXXFLAGS=$(sed -E 's#-std=(c|gnu)[0-9a-z]+#-std=c++17#; s#-Werror=(implicit-function-declaration|missing-prototypes|incompatible-pointer-types|int-conversion) *##g' <<<"$CC")
CXXFLAGS="$CXXFLAGS -I$MESA/src -I$POLY_BUILD/src -Wno-invalid-offsetof ${POLY_EXTRA_CFLAGS:-}"
AR=$(command -v llvm-ar || command -v gcc-ar || command -v ar)

# 2b. Our tessellator kernels (T2 step 1, src/mali_tess.cl): the three KERNEL(64) wrappers around Mesa's
#     poly_tess_*_process(), compiled by the host mesa_clc that the configure above just built, against the same
#     unmodified src/poly headers as libpoly. Output: out/<target>/poly/mali_tess.spv (one module, three kernels).
CL=$V/src/mali_tess.cl
if [ -f "$CL" ]; then
   mkdir -p "$OUT"
   t0=$SECONDS
   # The flags are Mesa's own cl_args (meson.build:67): CL 2.0 and the builtins the tessellator uses.
   "$POLY_BUILD/src/compiler/clc/mesa_clc" -o "$OUT/mali_tess.spv" "$CL" -- \
      -I"$MESA/include" -I"$MESA/src/compiler/libcl" -I"$MESA/src" \
      -cl-std=cl2.0 -D__OPENCL_VERSION__=200 -DHAVE___BUILTIN_FFS -DHAVE___BUILTIN_CLZ \
      >"$OUT/mali_tess.log" 2>&1 \
      || { echo "poly.sh: mesa_clc failed on $CL (see $OUT/mali_tess.log)"; tail -5 "$OUT/mali_tess.log" | sed 's/^/    /'; exit 1; }
   echo "poly: mali_tess.spv $(stat -c %s "$OUT/mali_tess.spv") B ($((SECONDS - t0)) s): mali_tess_{isoline,tri,quad}"
fi

# 3. Compile the unmodified sources for this target and archive them. Idempotent: nothing is redone when the archive is
#    newer than its inputs, so a repeated vk/build.sh costs one stat.
SRC="$MESA/src/poly/nir/poly_nir_lower_gs.c $MESA/src/poly/nir/poly_nir_lower_sysvals.c
     $MESA/src/poly/nir/poly_nir_lower_tess.c $MESA/src/poly/nir/poly_nir_lower_vs.c
     $MESA/src/poly/nir/poly_nir_passthrough_gs.c $POLY_BUILD/src/poly/cl/libpoly.cpp"
newest=$(ls -t $SRC "$MESA/src/poly/nir/poly_nir.h" "$POLY_BUILD/src/poly/cl/libpoly.h" | head -1)
if [ -f "$OUT/libpoly_nir.a" ] && [ "$OUT/libpoly_nir.a" -nt "$newest" ] && [ ${MALI_POLY_FORCE:-0} != 1 ]; then
   echo "poly: libpoly_nir.a up to date ($(stat -c %s "$OUT/libpoly_nir.a") B, $TARGET)"
   echo "$OUT/libpoly_nir.a"; exit 0
fi
mkdir -p "$OBJ"
cd "$B"   # the target build dir's -I paths are relative to it, as in build.sh
t0=$SECONDS
objs=()
for s in $SRC; do
   o=$OBJ/$(basename "${s%.*}").o
   case $s in *.cpp) eval "$CXXFLAGS -fPIC -o '$o' -c '$s'" ;; *) eval "$CFLAGS -fPIC -o '$o' -c '$s'" ;; esac
   objs+=("$o")
done
rm -f "$OUT/libpoly_nir.a"
"$AR" csr "$OUT/libpoly_nir.a" "${objs[@]}"
echo "poly: libpoly_nir.a $(stat -c %s "$OUT/libpoly_nir.a") B, ${#objs[@]} objects, $TARGET, $((SECONDS - t0)) s"
echo "$OUT/libpoly_nir.a"
