#!/bin/bash
# Builds spike against the meson build tree (reuses panfrost_compile's compile+link lines).
set -e
D=$(cd "$(dirname "$0")/.." && pwd); S=$D/compiler-spike; export PATH=$D/third_party/venv/bin:$PATH
cd $D/third_party/build
ninja -t commands src/panfrost/clc/panfrost_compile | grep -- "pan_compile.c$" > /tmp/cc.txt
ninja -t commands src/panfrost/clc/panfrost_compile | tail -1 > /tmp/ld.txt
CC=$(sed -e "s# -MD -MQ [^ ]*##; s# -MF [^ ]*##; s# -o [^ ]* -c [^ ]*pan_compile.c# -o $S/spike.o -c $S/spike.c#" /tmp/cc.txt)
eval "$CC"
LD=$(sed -e "s# -o src/panfrost/clc/panfrost_compile # -o $S/spike #; s#src/panfrost/clc/panfrost_compile.p/pan_compile.c.o#$S/spike.o#" /tmp/ld.txt)
eval "$LD"
