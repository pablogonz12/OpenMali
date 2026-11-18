#!/bin/bash
# SPDX-License-Identifier: MIT
# The shader cache key (mali_compile.c mkey) only knows MALI_COMPILE_VERSION: a change to the lowering that is not accompanied by a bump
# leaves stale code in every user's cache (4a10c07 moved the dynamic-buffer handles to table 9 and kept version 19: cached shaders kept
# addressing table 0). This guard records the sha256 of the sources that decide the generated code next to the version they were last
# reviewed at. Usage: compile-guard.sh [--bless]. Exit 0 = recorded state matches.
set -eu
V=$(cd "$(dirname "$0")/.." && pwd); G=$V/tests/compile-guard.txt
FILES="src/mali_compile.c src/mali_descriptor_lower.c src/mali_descriptor_lower.h src/mali_pipeline_layout_flat.h"
# the version line itself (and its comment) is excluded: bumping it must not change the hash
cur_ver=$(sed -n 's/^#define MALI_COMPILE_VERSION \([0-9]*\).*/\1/p' "$V/src/mali_compile.c")
cur_hash=$(cd "$V"; for f in $FILES; do grep -v '^#define MALI_COMPILE_VERSION ' "$f"; done | sha256sum | cut -d' ' -f1)
if [ "${1:-}" = --bless ]; then printf 'version %s\nsha256 %s\nfiles %s\n' "$cur_ver" "$cur_hash" "$FILES" >"$G"; echo "compile guard blessed: version $cur_ver"; exit 0; fi
[ -f "$G" ] || { echo "no $G (run $0 --bless)"; exit 1; }
rec_ver=$(sed -n 's/^version //p' "$G"); rec_hash=$(sed -n 's/^sha256 //p' "$G")
if [ "$rec_hash" = "$cur_hash" ] && [ "$rec_ver" = "$cur_ver" ]; then echo "compile guard: version $cur_ver matches the lowering sources"; exit 0; fi
if [ "$rec_ver" = "$cur_ver" ]; then
   echo "the shader-compile sources ($FILES) changed but MALI_COMPILE_VERSION is still $cur_ver:"
   echo "bump it in src/mali_compile.c if the generated code can differ (stale entries in users' caches!), then run ./check.sh --bless"
else
   echo "MALI_COMPILE_VERSION is $cur_ver, the guard records $rec_ver: run ./check.sh --bless to record the reviewed state"
fi
exit 1
