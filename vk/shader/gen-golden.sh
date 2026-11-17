#!/bin/bash
# SPDX-License-Identifier: MIT
# Regenerates golden_compiled_v13.h (the v11 golden is libkbase/tests/shaders_compiled_v11.h from compiler-spike/gen-compiled.sh) with the
# offline spike, same options as gen-compiled.sh. t_compile compares mali_compile() output byte for byte against these.
set -e; D=$(cd "$(dirname "$0")" && pwd); K=$D/../../compiler-spike; T=$(mktemp -d); export GEN_ARCH=13
cd "$K"
SPIKE_FAU_RESERVED=16 SPIKE_PUSH_BASE=32 ./spike 13 vert $D/rot.vert.spv $T/vs.bin 2>/dev/null > $T/vs.txt
./spike 13 frag $D/var.frag.spv $T/fs.bin 2>/dev/null > $T/fs.txt
python3 -I gen_header.py $D/golden_compiled_v13.h crot_vs:$T/vs.txt:$T/vs.bin crot_fs:$T/fs.txt:$T/fs.bin
sed -i 's#gen-shaders.sh#vk/shader/gen-golden.sh#' $D/golden_compiled_v13.h; sed -i 's/crot_/crot13_/g; s/CROT_/CROT13_/g' $D/golden_compiled_v13.h; rm -rf $T
