#!/bin/bash
# One ladder step on one phone, with the two guards that a shared machine needs:
#   * logs are pulled into /tmp/opencode/ladder-logs/<phone>/<session>/, never into a shared dir,
#     because `ddc logs latest` in a shared cwd can hand you another phone's session;
#   * the session's renderer must name the phone's GPU (cmf=G615, x8=G925, x4=G610), else WRONG DEVICE.
#   usage: gl-ladder-phone.sh <step> <phone> [hold_ms]
set -u
n=${1:?step (0..4)}; phone=${2:?phone}; hold=${3:-20000}
M=${MALI_ORCH:-$HOME/.config/opencode/mali-orchestrator}
WT=$(cd "$(dirname "$0")/.." && pwd)
case "$phone" in cmf) gpu=G615;; x8) gpu=G925;; x4) gpu=G610;; *) echo "unknown phone $phone"; exit 2;; esac
RD=/storage/emulated/0/Download/ladder-ready
LOGS=/tmp/opencode/ladder-logs/$phone
mkdir -p "$LOGS"
model=$(timeout 30 $M/bin/ddc "$phone" adb shell getprop ro.product.model 2>/dev/null | tr -d '\r')
echo "phone=$phone model=$model expect_gpu=$gpu"
[ -n "$model" ] || { echo "DEVICE OFFLINE"; exit 1; }
"$WT/tools/gl-ladder.sh" --cross >/dev/null || { echo "BUILD FAILED"; exit 1; }
timeout 90 $M/bin/ddc "$phone" adb push "$WT/gl-ladder-a64" /storage/emulated/0/Download/gl-ladder >/dev/null 2>&1 || { echo "PUSH FAILED"; exit 1; }
timeout 60 $M/bin/ddc "$phone" ui click --match Back >/dev/null 2>&1
timeout 90 $M/bin/ddc "$phone" stop >/dev/null 2>&1
timeout 30 $M/bin/ddc "$phone" adb shell "rm -f $RD" >/dev/null 2>&1
out=$(timeout 90 $M/bin/ddc "$phone" run sh -c "cp /storage/emulated/0/Download/gl-ladder /root/ && chmod +x /root/gl-ladder && LADDER_READY=$RD LADDER_STEP=$n LADDER_HOLD=$hold /root/gl-ladder" 2>&1)
sid=$(echo "$out" | grep -o '"sessionId": "[^"]*"' | cut -d'"' -f4)
[ -n "$sid" ] || { echo "NO SESSION: $(echo "$out" | tail -2)"; exit 1; }
for i in $(seq 1 40); do
  up=$(timeout 20 $M/bin/ddc "$phone" adb shell "cat $RD" 2>/dev/null | tr -d '\r')
  case "$up" in step=*) echo "ready=$up after ${i}s"; break;; esac
  sleep 1
done
for t in a b; do
  case $t in a) sleep 8;; b) sleep 12;; esac
  if ! timeout 90 $M/bin/ddc "$phone" screenshot "/tmp/opencode/ladder-$n-$t.png" >/dev/null 2>&1; then
     echo "SHOT REFUSED ($t)"; exit 3; fi
  magick "/tmp/opencode/ladder-$n-$t.png" -resize 460x "/tmp/opencode/ladder-$n-$t.jpg"
  magick "/tmp/opencode/ladder-$n-$t.png" -format "  shot=$t %wx%h mean=%[fx:int(mean*25500)/100] centre=%[pixel:p{w/2,h/2}]\n" info:
done
timeout 30 $M/bin/ddc "$phone" adb shell "rm -f $RD" >/dev/null 2>&1
timeout 200 $M/bin/ddc "$phone" wait idle --timeout 150 >/dev/null 2>&1
(cd "$LOGS" && timeout 90 $M/bin/ddc "$phone" logs latest >/dev/null 2>&1)
S=/tmp/opencode/ladder-logs/$phone/$sid/session.log
echo "session=$sid log=$S"
[ -f "$S" ] || { echo "NO LOG for $sid"; exit 1; }
r=$(grep -a -m1 'renderer="' "$S")
echo "renderer: ${r#*renderer=}"
case "$r" in *"$gpu"*) ;; *) echo "WRONG DEVICE: renderer does not name $gpu"; exit 4;; esac
grep -aE 'step=[0-9] |readback|direct_clear|blit=|swap=' "$S" | grep -av gamescope | sed 's/^/  /'
