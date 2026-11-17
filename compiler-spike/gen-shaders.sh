#!/bin/bash
# SPDX-License-Identifier: MIT
# GLSL -> SPIR-V -> (Mesa offline compiler, ./spike) -> Valhall v11 -> C header for libkbase tests.
# Usage: gen-shaders.sh [out.h] [arch 11|13]  (default: tests/shaders_v11.h, arch 11; arch 13 -> tests/shaders_v13.h)
# Needs glslangValidator and a built ./spike (see build.sh / NOTES.md). The test binaries need no compiler.
set -e
D=$(cd "$(dirname "$0")" && pwd); ARCH=${2:-11}; OUT=${1:-$D/../libkbase/tests/shaders_v$ARCH.h}; T=$(mktemp -d); export GEN_ARCH=$ARCH
cd $D
for s in const.frag var.frag pos.vert uni.vert uni.frag tex.frag; do glslangValidator -V --target-env vulkan1.1 $s -o $T/$s.spv >/dev/null; done
SPIKE_LAYOUT_OUT=$T/vs.layout ./spike $ARCH vert $T/pos.vert.spv $T/vs.bin   2>/dev/null > $T/vs.txt
# arch 13 (PanVK links the FS against the VS varying layout): FS reads varyings with LD_VAR_BUF_IMM, no attribute descriptor. v11 keeps LD_VAR_IMM + descriptor.
if [ "$ARCH" = 13 ]; then export SPIKE_LAYOUT_IN=$T/vs.layout; fi
./spike $ARCH frag $T/const.frag.spv $T/fsc.bin 2>/dev/null > $T/fsc.txt
./spike $ARCH frag $T/var.frag.spv $T/fsv.bin   2>/dev/null > $T/fsv.txt
# uniform shaders: user push constants. VS: FAU reserved 16 words, push block at byte 32 (words 0..5 = viewport sysvals); FS: reserved 8, push block at byte 0.
SPIKE_FAU_RESERVED=16 SPIKE_PUSH_BASE=32 ./spike $ARCH vert $T/uni.vert.spv $T/uvs.bin 2>/dev/null > $T/uvs.txt
SPIKE_FAU_RESERVED=8  SPIKE_PUSH_BASE=0  ./spike $ARCH frag $T/uni.frag.spv $T/ufs.bin 2>/dev/null > $T/ufs.txt
# texture FS: handle (table 1, tex 0, sampler 1) baked as immediate; FAU reserved 8.
SPIKE_FAU_RESERVED=8 SPIKE_PUSH_BASE=0 ./spike $ARCH frag $T/tex.frag.spv $T/ftex.bin 2>/dev/null > $T/ftex.txt
python3 -I $D/gen_header.py $OUT vs:$T/vs.txt:$T/vs.bin fs_const:$T/fsc.txt:$T/fsc.bin fs_var:$T/fsv.txt:$T/fsv.bin vs_uni:$T/uvs.txt:$T/uvs.bin fs_uni:$T/ufs.txt:$T/ufs.bin fs_tex:$T/ftex.txt:$T/ftex.bin
rm -rf $T; echo "wrote $OUT"
