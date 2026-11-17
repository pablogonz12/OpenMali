#!/bin/bash
# SPDX-License-Identifier: MIT
# Reproducible build-time compile for tests/t_compiled.c: rot.vert + var.frag (GLSL) -> SPIR-V -> Mesa offline compiler (./spike) -> Valhall v11
# -> tests/shaders_compiled_v11.h (binaries + FAU/immediates/preload metadata; names crot_vs / crot_fs). The test binaries need no compiler.
# Usage: gen-compiled.sh [out.h]   Needs glslangValidator and a built ./spike (build.sh). Output is deterministic (diff against the committed header).
set -e
D=$(cd "$(dirname "$0")" && pwd); OUT=${1:-$D/../libkbase/tests/shaders_compiled_v11.h}; T=$(mktemp -d); export GEN_ARCH=11
cd $D
for s in rot.vert var.frag; do glslangValidator -V --target-env vulkan1.1 $s -o $T/$s.spv >/dev/null; done
# VS: viewport sysvals = FAU words 0..5, driver-reserved 16 words, user push block at byte 32 (as uni.vert); FS: default reserved 8, interpolated varying.
SPIKE_FAU_RESERVED=16 SPIKE_PUSH_BASE=32 ./spike 11 vert $T/rot.vert.spv $T/vs.bin 2>/dev/null > $T/vs.txt
./spike 11 frag $T/var.frag.spv $T/fs.bin 2>/dev/null > $T/fs.txt
python3 -I $D/gen_header.py $OUT crot_vs:$T/vs.txt:$T/vs.bin crot_fs:$T/fs.txt:$T/fs.bin
rm -rf $T; echo "wrote $OUT"
