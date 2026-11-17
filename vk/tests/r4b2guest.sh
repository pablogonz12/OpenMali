#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest-side R4-B2 test via scripts/quick/qdrv. Needs Download/r4b2_{libvulkan_mali.so,<bin>,*.spv,args.txt}; args.txt = "<bin> [args...]".
D=/storage/emulated/0/Download; T=/tmp/r4b2; rm -rf $T; mkdir -p $T/s
cp $D/r4b2_libvulkan_mali.so $T/libvulkan_mali.so
for f in $D/r4b2_*.spv; do b=$(basename $f); cp $f $T/s/${b#r4b2_}; done
set -- $(cat $D/r4b2_args.txt); bin=$1; shift
cp $D/r4b2_$bin $T/$bin; chmod +x $T/$bin
cd $T; ok=1; timeout 200 $T/$bin --direct $T/libvulkan_mali.so --spv-dir $T/s "$@" >$T/log 2>&1 || ok=0
tr '\t' ' ' <$T/log | sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/'
echo "{\"test\":\"summary\",\"ok\":$([ $ok = 1 ] && echo true || echo false)}"
