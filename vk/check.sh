#!/bin/bash
# SPDX-License-Identifier: MIT
# Offline check of the ICD (no GPU, no phone). Prints one line per case and "OK" or "FAILED".
#   ./check.sh [--no-build] [-v]
set -uo pipefail
V=$(cd "$(dirname "$0")" && pwd)
OUT=$V/out/native
build=1; verbose=0
for a in "$@"; do
   case $a in --no-build) build=0 ;; -v) verbose=1 ;; *) echo "usage: check.sh [--no-build] [-v]"; exit 2 ;; esac
done
if [ $build = 1 ]; then "$V/build.sh" >/dev/null || { echo "FAILED: build"; exit 1; }; fi

fail=0
# Only our ICD, no implicit layers, so the result does not depend on the laptop's drivers.
# Number of DXVK-required core/1.1-1.3 features still false (of 47); lower it only with a passing test for the new feature.
DXVK_MISSING=26
LOADER_ENV="VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json VK_LOADER_LAYERS_DISABLE=~all~"

run() { # name, expected exit status, command...
   local name=$1 want=$2 out rc; shift 2
   out=$(env "$@" 2>&1); rc=$?
   if [ $rc -eq "$want" ]; then echo "ok    $name"; else echo "FAIL  $name (exit $rc, want $want)"; fail=1; fi
   if [ $verbose = 1 ] || [ $rc -ne "$want" ]; then echo "$out" | sed 's/^/        /'; fi
}

run "loader v11"         0 $LOADER_ENV MALI_BACKEND=fake MALI_FAKE_GPU=v11 "$OUT/t_icd" --expect-name "Mali-G615"
run "loader v13"         0 $LOADER_ENV MALI_BACKEND=fake MALI_FAKE_GPU=v13 "$OUT/t_icd" --expect-name "Mali-G925"
run "direct v11"         0 MALI_BACKEND=fake MALI_FAKE_GPU=v11 "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-name "Mali-G615"
run "direct v13"         0 MALI_BACKEND=fake MALI_FAKE_GPU=v13 "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-name "Mali-G925"
run "direct no backend"  0 -u MALI_BACKEND "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-none
run "direct bad fake gpu" 0 MALI_BACKEND=fake MALI_FAKE_GPU=v99 "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-none
for g in v11 v13; do run "vk_info $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_info" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "dxvk_caps $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_dxvk_caps" --direct "$OUT/libvulkan_mali.so" --expect-missing "$DXVK_MISSING"; done

# M4: memory, sync, queue (fake backend; MALI_FAKE_SLOW_US makes the fences really block).
TQ="$OUT/t_queue --direct $OUT/libvulkan_mali.so"
run "queue all v11"      0 MALI_BACKEND=fake MALI_FAKE_GPU=v11 $TQ
run "queue all v13 slow" 0 MALI_BACKEND=fake MALI_FAKE_GPU=v13 MALI_FAKE_SLOW_US=200 $TQ --submits 1000
for n in 1 50; do run "fault inject $n" 0 MALI_BACKEND=fake MALI_FAULT_INJECT=$n $TQ --only fault; done

# M5/M7: buffers, images, views, samplers, memory binding, descriptor sets, updates, templates (white-box, static link).
for g in v11 v13; do run "resources $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_resources"; done

# M9: client draws through the ICD (fake backend: API sequence, compile, CS build and submit; pixels only on hardware).
for g in v11 v13; do
   run "vk_draw $g"       0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 200
   run "vk_draw reset $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 1000 --loop-reset
done

# R4-B2: DXVK-required core feature tests (pixels only on hardware).
for g in v11 v13; do
   run "vk_feat $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_feat" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader"
done

# M11: compute through the ICD (fake backend: compile, CS build and submit; results only on hardware).
for g in v11 v13; do
   run "vk_compute $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_compute" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 200
   run "vk_meta $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_meta" --direct "$OUT/libvulkan_mali.so" --loop 20
done

# Exported symbols: the three loader entry points and nothing else.
syms=$(nm -D --defined-only "$OUT/libvulkan_mali.so" | awk '{print $3}' | grep -v '^$' | sort | tr '\n' ' ')
want="vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion "
if [ "$syms" = "$want" ]; then echo "ok    exported symbols"; else echo "FAIL  exported symbols: $syms"; fail=1; fi

# No LGPL libelf on the link line (R3-PLAN.md section 7).
if readelf -d "$OUT/libvulkan_mali.so" | grep NEEDED | grep -qi libelf; then echo "FAIL  libelf in NEEDED"; fail=1; else echo "ok    no libelf in NEEDED"; fi

# No PanVK code in the binary.
if nm "$OUT/libvulkan_mali.so" 2>/dev/null | grep -qi 'panvk'; then echo "FAIL  panvk symbols linked"; fail=1; else echo "ok    no panvk symbols"; fi

if command -v vulkaninfo >/dev/null; then
   out=$(env $LOADER_ENV MALI_BACKEND=fake MALI_FAKE_GPU=v13 vulkaninfo --summary 2>&1); rc=$?
   if [ $rc -eq 0 ] && grep -q "Mali-G925 MC11 (mali-own, fake backend)" <<<"$out"; then echo "ok    vulkaninfo --summary"; else echo "FAIL  vulkaninfo --summary (exit $rc)"; fail=1; echo "$out" | tail -20; fi
   out=$(env $LOADER_ENV MALI_BACKEND=fake MALI_FAKE_GPU=v11 vulkaninfo 2>&1); rc=$?
   if [ $rc -eq 0 ] && grep -q "maxImageDimension2D" <<<"$out"; then echo "ok    vulkaninfo (full)"; else echo "FAIL  vulkaninfo full (exit $rc)"; fail=1; echo "$out" | tail -20; fi
else
   echo "skip  vulkaninfo (not installed)"
fi

[ $fail = 0 ] && echo OK || { echo FAILED; exit 1; }
