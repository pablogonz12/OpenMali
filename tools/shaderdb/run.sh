#!/bin/bash
# SPDX-License-Identifier: MIT
# shaderdb run: compile a SPIR-V corpus for v11 and v13 and write a TSV (instr/regs/spills/time per shader).
#   tools/shaderdb/run.sh [-o out.tsv] [-a 11,13] [-n] [extra.spv|dir ...]     (-n: skip the build)
# Run it through mali-slot on shared hosts. Env knobs (MALI_OPT_GCM=1, ...) pass straight through to the compiler.
# Corpus: vk/shader/*.spv, bench/mdbench/shaders, compiler-spike, $SHADERDB_EXTRA (colon list of dirs, e.g. a deqp output dir),
# and SPIR-V carved from a local DXVK install (Proton) if found (not committed: zlib-licensed binaries, extracted at run time).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
OUT=/dev/stdout ARCHS=11,13 BUILD=1
while getopts "o:a:n" o; do case $o in o) OUT=$OPTARG;; a) ARCHS=$OPTARG;; n) BUILD=0;; *) exit 2;; esac; done
shift $((OPTIND - 1))
[ $BUILD = 1 ] && (cd "$D/vk" && ./build.sh >&2)
BIN=$D/vk/out/native/shaderdb
C=${SHADERDB_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/shaderdb-corpus}
mkdir -p -m 700 "$C"
[ -O "$C" ] && [ -d "$C" ] && [ ! -L "$C" ] || { echo "run.sh: cache dir $C must be a real directory owned by you" >&2; exit 1; }
LIST=$(mktemp); trap 'rm -f "$LIST"' EXIT
{ for f in "$D"/vk/shader/*.spv "$D"/bench/mdbench/shaders/*.spv "$D"/compiler-spike/*.spv; do [ -f "$f" ] && printf '%s\0' "$f"; done
   EX=${SHADERDB_EXTRA:-}
   IFS=: read -r -a EXA <<< "$EX"
   for e in ${EXA[@]+"${EXA[@]}"} "$@"; do [ -n "$e" ] || continue; if [ -d "$e" ]; then find -H "$e" -name '*.spv' -print0; elif [ -f "$e" ]; then printf '%s\0' "$e"; else echo "run.sh: no such corpus path: $e" >&2; exit 2; fi; done
   if [ -z "${SHADERDB_NO_DXVK:-}" ]; then
      DX=${SHADERDB_DXVK_DIR:-$(ls -d /usr/share/steam/compatibilitytools.d/*/files/lib/wine/dxvk/x86_64-windows 2>/dev/null | head -1 || true)}
      if [ -n "$DX" ] && [ -d "$DX" ]; then
         exec 9> "$C/.lock"; flock 9   # one run at a time checks/cleans/extracts/publishes the cache (concurrent runs would rm each other's .tmp)
         if [ ! -f "$C/dxvk/.complete" ]; then   # publish only after a successful extraction; partial runs are retried
            rm -rf "$C/dxvk.tmp" "$C/dxvk"
            DLLS=(); for dll in d3d11 d3d9 d3d8 dxgi d3d10core; do [ -f "$DX/$dll.dll" ] && DLLS+=("$DX/$dll.dll"); done   # d3d8.dll only exists from DXVK 2.4
            python3 -I "$D/tools/shaderdb/extract_spv.py" "$C/dxvk.tmp" dxvk ${DLLS[@]+"${DLLS[@]}"} >&2
            touch "$C/dxvk.tmp/.complete"; mv "$C/dxvk.tmp" "$C/dxvk"
         fi
         flock -u 9; exec 9>&-
         for f in "$C"/dxvk/*.spv; do [ -f "$f" ] && printf '%s\0' "$f"; done
      fi
   fi; } > "$LIST"   # group runs under set -e (not in a pipeline), so a failing find/extract aborts the run
SORTED=$(mktemp); trap 'rm -f "$LIST" "$SORTED"' EXIT
LC_ALL=C sort -zu "$LIST" > "$SORTED"
# the corpus goes in as a NUL-separated list file, not argv (a DXVK-sized corpus exceeds ARG_MAX: E2BIG)
SHADERDB_STRIP="$D/:$C/" MALI_SHADER_CACHE= "$BIN" -a "$ARCHS" -l "$SORTED" > "$OUT"
