#!/bin/bash
# SPDX-License-Identifier: MIT
# Build a PRIVATE libvulkan_mali.so whose Wayland WSI is compiled from a scratch copy of third_party/mesa's wsi_common_wayland.c with
# vk/mesa-patches applied (never touches the shared tree or its build). Usage: build-wsi-patched.sh <scratch dir> <out dir with the normal build>
# Prints the .so path on the last line; exit 77 = no Mesa build tree (skip).
set -eu
S=$1; OUT=$2
V=$(cd "$(dirname "$0")/.." && pwd); D=$V/..
B=$D/third_party/build-vk-x86-x11; [ -f "$B/build.ninja" ] || B=$D/third_party/build-vk
[ -f "$B/build.ninja" ] && [ -f "$D/third_party/mesa/src/vulkan/wsi/wsi_common_wayland.c" ] || exit 77
F=src/vulkan/wsi/wsi_common_wayland.c
cp -rs "$(readlink -f "$D/third_party/mesa")" "$S/mesa"; rm "$S/mesa/$F"; cp "$D/third_party/mesa/$F" "$S/mesa/$F"   # symlink overlay: ../mesa/... include paths of the build tree resolve
for p in "$V"/mesa-patches/*.patch; do   # only the patches that touch the WSI file
   grep -q "^+++ b/$F" "$p" && { patch -p1 -s -d "$S/mesa" <"$p" || { echo "patch $(basename "$p") does not apply"; exit 1; }; }
done
CC=$(cd "$B" && ninja -t commands src/vulkan/wsi/libvulkan_wsi.a | grep -m1 -- 'wsi_common_wayland\.c$' | sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##')
(cd "$B" && eval "$CC -o $S/wl.o -c $S/mesa/$F")
# private Mesa build dir: symlinks to everything, except libvulkan_wsi.a (copy with the Wayland object replaced); build.sh uses relative paths
PB=$S/build-vk; mkdir -p "$PB"
for e in "$B"/* "$B"/.[!.]*; do [ -e "$e" ] && ln -sfn "$e" "$PB/$(basename "$e")"; done
rm "$PB/src"; mkdir -p "$PB/src/vulkan"
for e in "$B"/src/*; do [ "$(basename "$e")" = vulkan ] || ln -sfn "$e" "$PB/src/$(basename "$e")"; done
for e in "$B"/src/vulkan/*; do [ "$(basename "$e")" = wsi ] || ln -sfn "$e" "$PB/src/vulkan/$(basename "$e")"; done
mkdir -p "$PB/src/vulkan/wsi"
for e in "$B"/src/vulkan/wsi/*; do ln -sfn "$e" "$PB/src/vulkan/wsi/$(basename "$e")"; done
rm "$PB/src/vulkan/wsi/libvulkan_wsi.a"; cp "$B/src/vulkan/wsi/libvulkan_wsi.a" "$PB/src/vulkan/wsi/"
cp "$S/wl.o" "$S/wsi_common_wayland.c.o"; (cd "$PB/src/vulkan/wsi" && ar d libvulkan_wsi.a libvulkan_wsi.a.p/wsi_common_wayland.c.o && ar rT libvulkan_wsi.a "$S/wsi_common_wayland.c.o")
rm -rf "$V/out/native-wsi"; mkdir -p "$V/out/native-wsi"; cp -r "$OUT/obj" "$OUT/gen" "$V/out/native-wsi/"
MALI_RELINK=1 MESA_SRC=$S/mesa MESA_BUILD=$PB MALI_TARGET=native-wsi "$V/build.sh" >"$S/build.log" 2>&1 || { tail -20 "$S/build.log"; exit 1; }
echo "$V/out/native-wsi"
