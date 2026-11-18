#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest-side validation via qdrv: t_vk_draw --loop 20, t_vk_compute, t_vk_meta with env from Download/val_env.txt (e.g. "MALI_HF=330").
D=/storage/emulated/0/Download; T=/tmp/val; rm -rf $T; mkdir -p $T/s
cp $D/val_libvulkan_mali.so $T/libvulkan_mali.so
for t in t_vk_draw t_vk_compute t_vk_meta t_vk_feat; do cp $D/val_$t $T/$t; chmod +x $T/$t; done
for f in $D/val_*.spv; do b=$(basename $f); cp $f $T/s/${b#val_}; done
cd $T; ok=1; RAN=""
for e in $(cat $D/val_env.txt 2>/dev/null); do export $e; done
LOOP=$(cat $D/val_loop.txt 2>/dev/null || echo 20)
ONLY=$(cat $D/val_only.txt 2>/dev/null | tr ',' ' ')
DRAW=" triangle rot two depth load_depth load_depth16 load_stencil load_colour_f16 load_colour_a2b10 depth16 depth32s8 stencil poly idx16 idx32 mdi scissor viewport multivp clearatt fx msaa_resolve4 msaa_sample_shading ring occlusion pushfs ubo descupd texture int_clear_rgba8ui int_clear_rg16ui int_clear_r8ui rt_clear_r8snorm rt_clear_rgba8snorm rt_clear_a8b8g8r8snorm rt_clear_r16unorm rt_clear_rg16snorm rt_clear_r32f rt_clear_rgba32f rt_clear_a8b8g8r8ui rt_clear_a2b10g10r10ui "
META=" buf fill img copyimg blit clearc cleard resolve event ts "
FEAT=" storfmt_nf ycbcr spec linerast divisor "   # t_vk_feat cases the smoke set runs (Steam-session features; the rest of t_vk_feat is run by hand)
for c in "t_vk_draw --loop $LOOP" "t_vk_compute --loop 5" "t_vk_meta" "t_vk_feat"; do
  set -- $c; n=$1; shift
  sp="--spv-dir $T/s"; [ $n = t_vk_meta ] && sp=""
  if [ -z "$ONLY" ]; then CASES="-"; [ $n = t_vk_feat ] && CASES=$FEAT; else CASES=""; K=$META; [ $n = t_vk_draw ] && K=$DRAW; [ $n = t_vk_feat ] && K=$FEAT
    for o in $ONLY; do m=0; case "$K" in *" $o "*) m=1;; esac; case "$n:$o" in t_vk_draw:fx_*) m=1;; esac   # fx_<name>: one t_vk_draw fx case
      [ $m = 1 ] && CASES="$CASES $o" && RAN="$RAN $o"; done; [ -n "$CASES" ] || continue; fi
  : >$T/$n.log; rc=0
  for o in $CASES; do OA=""; [ $o = - ] || OA="--only $o"
    timeout 120 $T/$n --direct $T/libvulkan_mali.so $sp "$@" $OA >>$T/$n.log 2>&1 || rc=$?
  done
  [ $rc = 0 ] || ok=0
  echo "{\"test\":\"$n\",\"rc\":$rc,\"fail_lines\":$(grep -c '"fail"' $T/$n.log),\"pass_lines\":$(grep -c '"pass":true' $T/$n.log)}"
  grep '"fail"' $T/$n.log | head -3 | sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/'
  tail -200 $T/$n.log | sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"log":"'$n'","line":"\1"}/'
done
# an --only name no binary knows must not pass silently (a missing name in DRAW/META once gave a vacuous farm pass)
for o in $ONLY; do case " $RAN " in *" $o "*) ;; *) ok=0; echo "{\"fail\":\"valguest: unknown --only case $o (not in DRAW/META/FEAT)\"}";; esac; done
echo "{\"test\":\"summary\",\"ok\":$([ $ok = 1 ] && echo true || echo false)}"
