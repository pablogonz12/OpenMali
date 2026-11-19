#!/bin/bash
# Phone side of the AFBC hardware test; run it INSIDE `farm hold <dev> --max 20 -- vk/tests/afbc_hw_run.sh` (hold sets ANDROID_SERIAL).
# Needs vk/build.sh output for a64-bionic. Prints both logs; exit 0 only if the reference and the AFBC run both PASS.
set -u
cd "$(dirname "$0")/.." || exit 2
O=out/a64-bionic; D=/data/local/tmp/afbc_hw
for f in $O/libvulkan_mali.so $O/t_afbc_hw shader/rot.vert.spv shader/var.frag.spv shader/tex.vert.spv shader/tex.frag.spv; do [ -f "$f" ] || { echo "missing $f (run vk/build.sh)"; exit 2; }; done
adb shell "rm -rf $D; mkdir -p $D/s" && adb push $O/libvulkan_mali.so $O/t_afbc_hw $D/ >/dev/null &&
adb push shader/rot.vert.spv shader/var.frag.spv shader/tex.vert.spv shader/tex.frag.spv $D/s/ >/dev/null && adb shell "chmod +x $D/t_afbc_hw" || { echo "push failed"; exit 2; }
adb shell "cd $D && ./t_afbc_hw --direct $D/libvulkan_mali.so --spv-dir s >ref.log 2>&1; echo \$? >ref.rc"
echo "== reference (linear) =="; adb shell "cat $D/ref.log; echo rc=\$(cat $D/ref.rc)"
if [ "$(adb shell "cat $D/ref.rc" | tr -d '\r')" != 0 ]; then echo "reference failed: device/driver problem, not AFBC; stopping"; exit 1; fi
adb shell "cd $D && MALI_AFBC=1 timeout 60 ./t_afbc_hw --direct $D/libvulkan_mali.so --spv-dir s >afbc.log 2>&1; echo \$? >afbc.rc"
echo "== MALI_AFBC=1 =="; adb shell "cat $D/afbc.log; echo rc=\$(cat $D/afbc.rc)"
# loadOp LOAD (docs/afbc-default.md): reference must render and preserve; AFBC must be refused cleanly today, and must match the
# reference pixel counts once the AFBC preload path exists. Both halves exit 0 as long as the driver refuses instead of corrupting.
adb shell "cd $D && ./t_afbc_hw --direct $D/libvulkan_mali.so --spv-dir s --case load >load.log 2>&1; echo \$? >load.rc"
echo "== load reference (linear) =="; adb shell "cat $D/load.log; echo rc=\$(cat $D/load.rc)"
adb shell "cd $D && MALI_AFBC=1 timeout 60 ./t_afbc_hw --direct $D/libvulkan_mali.so --spv-dir s --case load >loadafbc.log 2>&1; echo \$? >loadafbc.rc"
echo "== load MALI_AFBC=1 (expect refused) =="; adb shell "cat $D/loadafbc.log; echo rc=\$(cat $D/loadafbc.rc)"
echo "== kernel =="; adb shell "dmesg 2>/dev/null | grep -i 'mali\|kbase\|fault' | tail -15"
[ "$(adb shell "cat $D/afbc.rc" | tr -d '\r')" = 0 ] && [ "$(adb shell "cat $D/load.rc" | tr -d '\r')" = 0 ] && [ "$(adb shell "cat $D/loadafbc.rc" | tr -d '\r')" = 0 ]
