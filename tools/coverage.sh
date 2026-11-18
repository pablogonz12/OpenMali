#!/bin/bash
# SPDX-License-Identifier: MIT
# gcov function coverage of the host tests on the fake backend (no lcov needed).
#   tools/coverage.sh build    build the instrumented driver (vk/out/native-cov), several minutes
#   tools/coverage.sh run      run vk/check.sh against it (deletes old .gcda first); needs a prior build
#   tools/coverage.sh report   per-file line coverage + functions never executed, biggest first (hot files excluded unless ALL=1)
# Caveat: only the driver objects are instrumented; the standalone build-t_*.sh tests (t_bc, t_meta, t_desc, ...) link their own
# copies and are not counted, and kbase-only code never runs on the fake backend.
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd); O=$D/vk/out/native-cov
case ${1:-} in
   build) MALI_COV=1 MALI_TARGET=native-cov "$D/vk/build.sh" ;;
   run) find "$O" -name '*.gcda' -delete; MALI_COV=1 "$D/vk/check.sh" --no-build ;;
   report)
      cd "$(mktemp -d)"
      for f in "$O"/obj/mali_*.gcda; do
         gcov -f -n -o "$O/obj" "$f" 2>/dev/null | awk -v F="$(basename "$f" .gcda)" \
            '/^Function/{gsub(/\x27/,"",$2);fn=$2} /^Lines executed/{split($2,a,":");print F, fn, a[2]+0, $4}'
      done > funcs.txt
      awk '{t[$1]+=$4; c[$1]+=$4*$3/100} END{for(f in t) printf "%-24s %3.0f%% of %d lines\n",f,100*c[f]/t[f],t[f]}' funcs.txt | sort
      echo "--- functions never executed (file function lines)"
      awk '$3==0' funcs.txt | { if [ "${ALL:-0}" = 1 ]; then cat; else grep -Ev '^mali_(cmd|cs_emit|pipeline) '; fi; } | sort -k4 -nr | head -${TOP:-60} ;;
   *) echo "usage: coverage.sh build|run|report"; exit 2 ;;
esac
