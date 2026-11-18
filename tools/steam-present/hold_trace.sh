#!/bin/bash
# runs INSIDE `farm hold poco-x8`; ANDROID_SERIAL is set by the hold. arg1 = libvulkan_mali.so, arg2 = outdir
set -u
export POCO_SERIAL=$ANDROID_SERIAL
LIB=$1; OUT=$2; mkdir -p $OUT
export PATH=$PATH:$HOME/.local/bin
CTL="python3 -I $HOME/Projects/mali-gaming/tools/droiddeckctl"
Q=$HOME/Projects/mali-gaming/scripts/quick
G() { $CTL guest --timeout 60 -- sh -c "$1"; }
st=$($CTL state | python3 -c 'import json,sys;s=json.load(sys.stdin)["session"];print(s.get("running"),s.get("phase"))')
echo "pre-state: $st"
case "$st" in *True*) echo FOREIGN-SESSION; exit 6;; esac
adb push "$LIB" /storage/emulated/0/Download/x11wsi_libvulkan_mali.so >/dev/null
# 1. bridge session just to install the ICD in the guest (/tmp/x11wsi persists)
$CTL start steam --wait --timeout 150 >/dev/null 2>&1
G 'mkdir -p /tmp/x11wsi && cp /storage/emulated/0/Download/x11wsi_libvulkan_mali.so /tmp/x11wsi/libvulkan_mali.so && printf "{\"file_format_version\":\"1.0.1\",\"ICD\":{\"library_path\":\"/tmp/x11wsi/libvulkan_mali.so\",\"api_version\":\"1.3.0\"}}" >/tmp/x11wsi/icd.json; rm -f /tmp/x11wsi/trace.log; sha1sum /tmp/x11wsi/libvulkan_mali.so'
$CTL stop >/dev/null 2>&1
# 2. our driver, GPU compositing, trace
$CTL prefs set session vkBridge false --type bool >/dev/null
$CTL env set BL_VK_DRIVER=/tmp/x11wsi/icd.json MALI_WSI=sw BL_STEAM_SOFT_COMPOSITING=0 MALI_TRACE=/tmp/x11wsi/trace.log >/dev/null
$CTL start steam --wait --timeout 120 >$OUT/start.txt 2>&1
sleep 40
$CTL state | python3 -c 'import json,sys;s=json.load(sys.stdin)["session"];print("phase",s.get("phase"))' >$OUT/phase.txt
G 'wc -l /tmp/x11wsi/trace.log; grep -c "^acquire" /tmp/x11wsi/trace.log; grep -c "^present" /tmp/x11wsi/trace.log; cp /tmp/x11wsi/trace.log /storage/emulated/0/Download/x11wsi_trace.log' >$OUT/counts.txt 2>&1
G 'for p in $(pgrep -f steamwebhelper | head -3); do echo pid $p; for t in /proc/$p/task/*; do echo "$(cat $t/comm) $(cat $t/wchan 2>/dev/null)"; done; done | sort | uniq -c | sort -rn | head -20' >$OUT/threads.txt 2>&1
adb pull /storage/emulated/0/Download/x11wsi_trace.log $OUT/trace.log >/dev/null 2>&1
$CTL logs latest $OUT/sessionlogs >/dev/null 2>&1
# 3. restore
bash $Q/qrestore poco >$OUT/restore.txt 2>&1 || $CTL stop >/dev/null 2>&1
echo done
