#!/bin/bash
# SPDX-License-Identifier: MIT
# Extra-warning report (no link, no run, nothing is fatal): compiles every ICD source with the flags build.sh takes from the Mesa tree
# plus a gcc+clang-portable set, and prints "file:line: warning [-Wxxx]" lines.
#   ./warnings.sh [--cc gcc|clang] [--conv]     --conv adds -Wconversion (noisy; off by default)
# Output: vk/out/warnings/<cc>.log; the last line is the count per flag.
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd); V=$D/vk
cc=gcc; conv=0
while [ $# -gt 0 ]; do case $1 in --cc) cc=$2; shift 2 ;; --conv) conv=1; shift ;; *) echo "usage: warnings.sh [--cc gcc|clang] [--conv]"; exit 2 ;; esac; done
MESA=${MESA_SRC:-$D/third_party/mesa}; B=${MESA_BUILD:-$D/third_party/build-vk}
[ -z "${MESA_BUILD:-}" ] && [ -f "$D/third_party/build-vk-x86-x11/build.ninja" ] && B=$D/third_party/build-vk-x86-x11
export PATH=$D/third_party/venv/bin:$PATH
OUT=$V/out/warnings; GEN=$OUT/gen; mkdir -p "$GEN"
"$D/third_party/venv/bin/python" "$MESA/src/vulkan/util/vk_entrypoints_gen.py" --xml "$MESA/src/vulkan/registry/vk.xml" --proto --weak \
   --out-h "$GEN/mali_entrypoints.h" --out-c "$GEN/mali_entrypoints.c" --prefix mali --device-prefix mali_v10 --device-prefix mali_v11 \
   --device-prefix mali_v13 --beta false
cd "$B"
CMDS=$(ninja -t commands src/panfrost/vulkan/libvulkan_panfrost.so)   # not piped into grep -m1: SIGPIPE + pipefail
CCLINE=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS")
CC=$(sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g' <<<"$CCLINE")
CC=$(sed -E "s#^[^ ]+#$cc#; s# -W[a-z-]*=?error[^ ]*##g" <<<"$CC")
# Portable subset: both compilers know these. -Wno-sign-conversion/-Wno-missing-field-initializers: designated-init and mixed-sign idioms
# are everywhere in Vulkan code (noise, not bugs).
W="-Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare"
[ $conv = 1 ] && W="$W -Wconversion -Wno-sign-conversion"
KB=$D/libkbase
: >"$OUT/$cc.log"
for s in $V/src/*.c $KB/src/*.c; do
   case $s in */mali_entrypoints.c) continue ;; esac
   eval "$CC -Wno-unknown-warning-option -fdiagnostics-color=never -I$GEN -I$V/src -I$KB/include -I$KB/src $W -fsyntax-only $s" >>"$OUT/$cc.log" 2>&1 || true
done
grep -v "/mesa/src" "$OUT/$cc.log" >"$OUT/$cc.log.f" 2>/dev/null || true
# Mesa's own headers are not ours to fix: report only our files.
grep -E '^(/|\.\./)?[^ ]*(vk/src|libkbase)/[^ ]*: warning:' "$OUT/$cc.log" | sort -u >"$OUT/$cc.ours.log" || true
echo "$(wc -l <"$OUT/$cc.ours.log") warnings in our files ($cc), $OUT/$cc.ours.log"
grep -o '\[-W[a-z-]*\]' "$OUT/$cc.ours.log" | sort | uniq -c | sort -rn || true
