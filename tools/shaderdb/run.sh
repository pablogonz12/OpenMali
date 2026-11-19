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
C=${SHADERDB_CACHE:-${TMPDIR:-/tmp}/shaderdb-corpus}
mkdir -p "$C"
FILES=$( { ls "$D"/vk/shader/*.spv "$D"/bench/mdbench/shaders/*.spv "$D"/compiler-spike/*.spv 2>/dev/null
   EX=${SHADERDB_EXTRA:-}
   for e in ${EX//:/ } "$@"; do if [ -d "$e" ]; then find "$e" -name '*.spv'; else echo "$e"; fi; done
   if [ -z "${SHADERDB_NO_DXVK:-}" ]; then
      DX=${SHADERDB_DXVK_DIR:-$(ls -d /usr/share/steam/compatibilitytools.d/*/files/lib/wine/dxvk/x86_64-windows 2>/dev/null | head -1)}
      if [ -n "$DX" ] && [ -d "$DX" ]; then
         [ -d "$C/dxvk" ] || python3 -I "$D/tools/shaderdb/extract_spv.py" "$C/dxvk" dxvk "$DX"/d3d11.dll "$DX"/d3d9.dll "$DX"/d3d8.dll "$DX"/dxgi.dll "$DX"/d3d10core.dll >&2
         ls "$C"/dxvk/*.spv 2>/dev/null
      fi
   fi; } | LC_ALL=C sort -u)
# shellcheck disable=SC2086
MALI_SHADER_CACHE= "$BIN" -a "$ARCHS" $FILES > "$OUT"
