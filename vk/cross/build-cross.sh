#!/bin/bash
# SPDX-License-Identifier: MIT
# Cross builds of Mesa's static libs + our ICD: ./cross/build-cross.sh [glibc|bionic|all]
#   out/a64-glibc/  (guest, Ubuntu arm64 sysroot of droidtest/tools)   out/a64-bionic/ (adb shell, NDK r27d, API 29)
# Prereq: native build-vk exists (README.md); its host tools mesa_clc, vtn_bindgen2, panfrost_compile are built here.
set -euo pipefail
V=$(cd "$(dirname "$0")/.." && pwd); TP=$V/../third_party; X=$V/cross
export PATH=$TP/venv/bin:$TP/build-vk/src/compiler/clc:$TP/build-vk/src/compiler/spirv:$TP/build-vk/src/panfrost/clc:$PATH
ninja -C "$TP/build-vk" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile
COMMON="-Dbuildtype=release -Dgallium-drivers= -Dvulkan-drivers=panfrost -Dtools= -Dglx=disabled -Dplatforms= -Degl=disabled
  -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled
  -Dpanfrost-rust=false -Dmesa-clc=system -Dprecomp-compiler=system -Db_ndebug=false"
LIBS="src/vulkan/runtime/libvulkan_lite_runtime.a src/vulkan/runtime/libvulkan_runtime.a src/vulkan/runtime/libvulkan_instance.a
  src/vulkan/wsi/libvulkan_wsi.a src/vulkan/util/libvulkan_util.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
  src/compiler/spirv/libvtn.a src/util/libmesa_util.a src/util/blake3/libblake3.a"
what=${1:-all}
if [ "$what" = glibc ] || [ "$what" = all ]; then
  B=$TP/build-vk-a64-glibc
  [ -f $B/build.ninja ] || meson setup $B $TP/mesa --cross-file $X/a64-glibc.ini $COMMON -Dxmlconfig=enabled
  # the PanVK .so is only the convenient target that pulls in every static library (it is not shipped)
  ninja -C $B src/panfrost/vulkan/libvulkan_panfrost.so
  MESA_BUILD=$B MALI_TARGET=a64-glibc "$V/build.sh"
fi
if [ "$what" = bionic ] || [ "$what" = all ]; then
  B=$TP/build-vk-a64-bionic
  # libdrm comes from Mesa's own wrap (needs network once); the NDK lacks cutils/native_handle.h, pthread_cancel
  # and sync_wait: cross/shim + cross/ndk-c force-include them, Mesa itself stays unmodified (0 patches).
  [ -f $B/build.ninja ] || meson setup $B $TP/mesa --cross-file $X/a64-bionic.ini --wrap-mode=default \
     -Dallow-fallback-for=libdrm $COMMON -Dxmlconfig=disabled -Dzstd=disabled -Dandroid-libbacktrace=disabled \
     -Dandroid-libperfetto=disabled -Dandroid-strict=false
  # the .so link of PanVK fails (liblog, libdrm): all static libs are built by then, which is all we need
  # EXPECTED failure (ld.lld: undefined __android_log_write / sync_merge): only the PanVK oracle .so fails; the static libs checked below are what counts
  ninja -k 0 -C $B src/panfrost/vulkan/libvulkan_panfrost.so || echo "note: PanVK oracle .so link failed as expected (see comment); continuing"
  for l in $LIBS; do [ -f $B/$l ] || { echo "missing $B/$l"; exit 1; }; done
  MALI_EXTRA_LD="-static-libstdc++ -llog -lsync $B/subprojects/libdrm-2.4.133/libdrm.so.p/*.o" \
     MESA_BUILD=$B MALI_TARGET=a64-bionic "$V/build.sh"
fi
