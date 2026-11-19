#!/bin/bash
# SPDX-License-Identifier: MIT
# Run the API fuzz harness against the ASan+UBSan host build of the ICD (fake backend, no GPU).
#   tools/fuzz/run.sh [--no-build] [--mode spv|structs|all] [--seed S] [--iters N] [--time-box SEC] [extra vk_fuzz args]
# Run it under the farm slot limiter:  mali-slot -- tools/fuzz/run.sh --mode structs
# Output: $FUZZ_OUT (default: mktemp -d) with vk_fuzz binary, findings.txt, and one log per finding (seed-<N>.log).
set -uo pipefail
D=$(cd "$(dirname "$0")" && pwd); V=$D/../../vk
build=1; mode=structs; seed=1; iters=1000000; box=300; extra=()
while [ $# -gt 0 ]; do
   case $1 in
      --no-build) build=0 ;;
      --mode) mode=$2; shift ;;
      --seed) seed=$2; shift ;;
      --iters) iters=$2; shift ;;
      --time-box) box=$2; shift ;;
      *) extra+=("$1") ;;
   esac
   shift
done
export MALI_SAN=1 MALI_TARGET=native-san
ICD=$V/out/native-san/libvulkan_mali.so
if [ $build = 1 ] || [ ! -f "$ICD" ]; then "$V/build.sh" >/dev/null || { echo "build failed"; exit 1; }; fi
OUT=${FUZZ_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/vk-fuzz.XXXXXX")}; mkdir -p "$OUT"
gcc -O1 -g -o "$OUT/vk_fuzz" "$D/vk_fuzz.c" -ldl || exit 1
export MALI_BACKEND=fake MALI_FAKE_GPU=${MALI_FAKE_GPU:-v13}
export MALI_SHADER_CACHE=$OUT/shader-cache
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export LD_PRELOAD=$(gcc -print-file-name=libasan.so)
SPV=${FUZZ_SPV_DIR:-$V/out/shader}; [ -d "$SPV" ] || SPV=$V/../bench/mdbench/shaders
echo "fuzz: mode=$mode seed=$seed time-box=${box}s out=$OUT spv=$SPV"
# The harness forks per iteration; findings are printed with their seed. The time box kills the run (exit 124 is not a finding).
timeout -k 5 "$box" "$OUT/vk_fuzz" --icd "$ICD" --spv-dir "$SPV" --mode "$mode" --seed "$seed" --iters "$iters" "${extra[@]}" >"$OUT/findings.txt" 2>"$OUT/stderr.txt"
rc=$?
grep -c '^FINDING' "$OUT/findings.txt" | sed 's/^/findings: /'
# Re-run each finding in-process to capture the sanitizer report.
for s in $(sed -n 's/^FINDING seed=\([0-9]*\) .*/\1/p' "$OUT/findings.txt" | sort -un | head -${MAX_TRIAGE:-20}); do
   timeout -k 2 30 "$OUT/vk_fuzz" --icd "$ICD" --spv-dir "$SPV" --mode "$mode" "${extra[@]}" --seed "$s" --iters 1 --no-fork >"$OUT/seed-$s.log" 2>&1
   echo "seed $s: $(grep -m1 -E 'ERROR: AddressSanitizer|runtime error|Assertion|SUMMARY' "$OUT/seed-$s.log")"
done
[ $rc = 124 ] && echo "time box reached"
# 124 = time box (normal); 126/127 = timeout could not launch the harness (missing binary/timeout): a failed run, not a clean one
[ $rc = 126 ] || [ $rc = 127 ] && { echo "fuzz: harness failed to launch (status $rc)"; exit "$rc"; }
exit 0
