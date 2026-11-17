#!/bin/bash
# Builds kdecode (pandecode over a .mem snapshot) against the meson build tree, like build.sh does for spike.
set -e
D=$(cd "$(dirname "$0")/.." && pwd); S=$D/compiler-spike; export PATH=$D/third_party/venv/bin:$PATH
cd $D/third_party/build
ninja -t commands src/panfrost/clc/panfrost_compile | grep -- "pan_compile.c$" > /tmp/kd_cc.txt
ninja -t commands src/panfrost/clc/panfrost_compile | tail -1 > /tmp/kd_ld.txt
CC=$(sed -e "s# -MD -MQ [^ ]*##; s# -MF [^ ]*##; s# -o [^ ]* -c [^ ]*pan_compile.c# -o $S/kdecode.o -c $S/kdecode.c#" /tmp/kd_cc.txt)
eval "$CC"
LD=$(sed -e "s# -o src/panfrost/clc/panfrost_compile # -o $S/kdecode #; s#src/panfrost/clc/panfrost_compile.p/pan_compile.c.o#$S/kdecode.o#" /tmp/kd_ld.txt)
eval "$LD"
