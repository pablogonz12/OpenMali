#!/bin/bash
# SPDX-License-Identifier: MIT
# Host x86-64 dEQP-VK against the native fake-backend ICD with ASan+UBSan, both fake GPUs (v11, v13). Slow, separate from check.sh.
# Compares ONLY Crash / Timeout / NotSupported with host-baseline.tsv and prints NEW ones by test name (exit 1 if any).
#   ./host-run.sh [--bless] [--fresh] [--limit N] [--gpus v11,v13] [--chunk 100] [--timeout 300] [--list FILE]
# Prereqs: ../build.sh with MALI_SAN=1 MALI_TARGET=native-san  (-> out/native-san/libvulkan_mali.so), and the host deqp-vk build:
#   third_party/build-vk-x86-host  (cmake -G Ninja, clang, headless; see README "Host build"). Needs nice -n 15 -j3 on a shared machine.
# --bless writes host-baseline.tsv from this run (review with git diff, commit with the change that caused it).
# Per-batch timeout (default 300 s per 100 cases): the case in flight is recorded as Timeout, the run resumes after it.
# Results are kept in out/host-deqp/ (results-<gpu>.tsv, resumable; --fresh restarts).
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); V=$H/..
TP=${MALI_THIRD_PARTY:-$V/../third_party}
DEQP=${DEQP_VK:-$TP/build-vk-x86-host/external/vulkancts/modules/vulkan/deqp-vk}
LIST=$H/lists/daily-sample.txt
[ -f "$LIST" ] || LIST=$HOME/Documents/scripts/mali-open/vk/deqp/lists/daily-sample.txt   # lists/ is untracked (regenerable: lists/gen.py)
args=(); while [ $# -gt 0 ]; do case $1 in --list) LIST=$2; shift 2;; *) args+=("$1"); shift;; esac; done
SAN=$V/out/native-san
[ -x "$DEQP" ] || { echo "no host deqp-vk at $DEQP (build it: see header)"; exit 2; }
[ -f "$SAN/libvulkan_mali.so" ] || { echo "no sanitizer driver: (cd $V && MALI_SAN=1 MALI_TARGET=native-san ./build.sh)"; exit 2; }
OUT=$V/out/host-deqp; mkdir -p "$OUT"
ICD=$OUT/icd.json
printf '{"file_format_version":"1.0.1","ICD":{"library_path":"%s","api_version":"1.3.0"}}\n' "$(realpath "$SAN/libvulkan_mali.so")" >"$ICD"
exec python3 -I "$H/host-run.py" --deqp "$DEQP" --icd "$ICD" --asan "$(gcc -print-file-name=libasan.so)" --list "$LIST" --out "$OUT" \
     --baseline "$H/host-baseline.tsv" "${args[@]}"
