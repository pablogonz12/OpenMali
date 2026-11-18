#!/bin/bash
# SPDX-License-Identifier: MIT
# Cross builds of Mesa's static libs + our ICD: ./cross/build-cross.sh [glibc|bionic|glibc-wl|glibc-wsi|all]
#   out/a64-glibc/  (guest, Ubuntu arm64 sysroot of droidtest/tools)   out/a64-bionic/ (adb shell, NDK r27d, API 29)
# Prereq: native build-vk exists (README.md); its host tools mesa_clc, vtn_bindgen2, panfrost_compile are built here.
set -euo pipefail
V=$(cd "$(dirname "$0")/.." && pwd); TP=$V/../third_party; X=$V/cross
# third_party is untracked and lives in the main checkout: link it into a fresh worktree (no absolute path in the repo)
if [ ! -e "$TP" ]; then
  MAIN=$(dirname "$(git -C "$V" rev-parse --path-format=absolute --git-common-dir)")
  [ -d "$MAIN/third_party" ] || { echo "no third_party in $V/.. or main checkout $MAIN"; exit 1; }
  ln -s "$MAIN/third_party" "$TP"
fi
DT=${MALI_DROIDTEST:-$HOME/Projects/mali-gaming/droidtest}
# meson machine files are generated from the .ini.in templates so no absolute path is committed. They live at a path that never
# moves (shared third_party/cross-ini, remembered by the Mesa build dirs) and point at the MAIN checkout's wrappers (cross/), so
# removing a worktree cannot make them stale. A build dir that remembers a missing cross file is re-pointed once (--reconfigure).
MAIN=$(dirname "$(git -C "$V" rev-parse --path-format=absolute --git-common-dir)")
[ -x "$MAIN/vk/cross/ndk-c" ] && XM=$MAIN/vk/cross || XM=$X
INI=$TP/cross-ini
mkdir -p "$INI"
for t in a64-glibc a64-bionic; do  # rewritten only on change: a newer mtime would make meson regenerate (and re-warn) every run
  sed "s#@CROSS@#$XM#g; s#@DROIDTEST@#$DT#g" "$X/$t.ini.in" >"$INI/$t.ini.new"
  cmp -s "$INI/$t.ini.new" "$INI/$t.ini" && rm "$INI/$t.ini.new" || mv "$INI/$t.ini.new" "$INI/$t.ini"
done
fixdir() { # <builddir> <ini>: a build dir remembers its machine file by path; re-point it when that file is gone (repo moved)
  [ -f "$1/build.ninja" ] || return 0
  "$TP/venv/bin/python" - "$1" "$2" <<'PY'
import sys, re, os
from mesonbuild import coredata
b, ini = sys.argv[1:3]
cd = coredata.load(b)
if all(os.path.exists(f) for f in cd.cross_files):
    sys.exit(0)
cd.cross_files = [ini]
coredata.save(cd, b)
p = b + "/meson-private/cmd_line.txt"
t = open(p).read()
open(p, "w").write(re.sub(r"cross_file = \[.*\]", "cross_file = ['%s']" % ini, t))
print("note: re-pointed %s to %s" % (b, ini))
PY
}
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
  B=$TP/build-vk-a64-glibc; fixdir $B $INI/a64-glibc.ini
  [ -f $B/build.ninja ] || meson setup $B $TP/mesa --cross-file $INI/a64-glibc.ini $COMMON -Dxmlconfig=enabled
  # the PanVK .so is only the convenient target that pulls in every static library (it is not shipped)
  ninja -C $B src/panfrost/vulkan/libvulkan_panfrost.so
  MESA_BUILD=$B MALI_TARGET=a64-glibc "$V/build.sh"
fi
if [ "$what" = glibc-wl ]; then   # (not in "all") the WSI variant (Wayland platform): what the guest benchmarks and games load
  B=$TP/build-vk-a64-glibc-wl; fixdir $B $INI/a64-glibc.ini
  [ -f $B/build.ninja ] || meson setup $B $TP/mesa --cross-file $INI/a64-glibc.ini ${COMMON/-Dplatforms=/-Dplatforms=wayland} -Dxmlconfig=enabled
  ninja -C $B src/panfrost/vulkan/libvulkan_panfrost.so
  MESA_BUILD=$B MALI_TARGET=a64-glibc-wl "$V/build.sh"
fi
if [ "$what" = glibc-wsi ]; then  # (not in "all") Wayland + X11 platforms: the guest's Steam clients are X11 clients of gamescope's Xwayland
  export PATH=$X/hostbin:$PATH   # host wayland-scanner reporting the sysroot libwayland version
  B=$TP/build-vk-a64-glibc-wsi; fixdir $B $INI/a64-glibc.ini
  [ -f $B/build.ninja ] || meson setup $B $TP/mesa --cross-file $INI/a64-glibc.ini ${COMMON/-Dplatforms=/-Dplatforms=wayland,x11} -Dxmlconfig=enabled
  ninja -C $B src/panfrost/vulkan/libvulkan_panfrost.so
  MESA_BUILD=$B MALI_TARGET=a64-glibc-wsi "$V/build.sh"
fi
if [ "$what" = bionic ] || [ "$what" = all ]; then
  B=$TP/build-vk-a64-bionic; fixdir $B $INI/a64-bionic.ini
  # libdrm comes from Mesa's own wrap (needs network once); the NDK lacks cutils/native_handle.h, pthread_cancel
  # and sync_wait: cross/shim + cross/ndk-c force-include them, Mesa itself stays unmodified (0 patches).
  [ -f $B/build.ninja ] || meson setup $B $TP/mesa --cross-file $INI/a64-bionic.ini --wrap-mode=default \
     -Dallow-fallback-for=libdrm $COMMON -Dxmlconfig=disabled -Dzstd=disabled -Dandroid-libbacktrace=disabled \
     -Dandroid-libperfetto=disabled -Dandroid-strict=false
  # the .so link of PanVK fails (liblog, libdrm): all static libs are built by then, which is all we need
  # EXPECTED failure (ld.lld: undefined __android_log_write / sync_merge): only the PanVK oracle .so fails; the static libs checked below are what counts
  ninja -k 0 -C $B src/panfrost/vulkan/libvulkan_panfrost.so || echo "note: PanVK oracle .so link failed as expected (see comment); continuing"
  for l in $LIBS; do [ -f $B/$l ] || { echo "missing $B/$l"; exit 1; }; done
  MALI_EXTRA_LD="-static-libstdc++ -llog -lsync $B/subprojects/libdrm-2.4.133/libdrm.so.p/*.o" \
     MESA_BUILD=$B MALI_TARGET=a64-bionic "$V/build.sh"
fi
