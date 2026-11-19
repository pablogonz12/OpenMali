#!/bin/bash
# SPDX-License-Identifier: MIT
# Host: validation on a phone (default poco). Usage: [PHONE=cmf|poco|poco-x4] valrun.sh "<ENV=.. ENV2=..>" [loop]
set -e; export PATH=$PATH:$HOME/Android/Sdk/platform-tools
V=$(cd "$(dirname "$0")/.." && pwd); PH=${PHONE:-poco}; case $PH in cmf|poco|poco-x4) ;; *) PH=poco;; esac; S=$("$V/../tools/dev-serial" "$PH");
# the phone's qsafe lock must be held by us or by an ancestor (farmd/qsafe); QSAFE_LOCK_OVERRIDE=1 for emergencies
if [ "${QSAFE_LOCK_OVERRIDE:-}" != 1 ]; then exec 8>>/tmp/qsafe-$PH.lock; if flock -n 8; then export QSAFE_HELD=$PH; elif [ "${QSAFE_HELD:-}" != "$PH" ]; then echo "PHONE BUSY: /tmp/qsafe-$PH.lock held by another process" >&2; exit 73; fi; fi; DL=/storage/emulated/0/Download
timeout 100 adb -s $S push $V/out/a64-glibc/libvulkan_mali.so $DL/val_libvulkan_mali.so >/dev/null
if [ -n "${VAL_SO_SHA256:-}" ]; then G=$(timeout 30 adb -s $S shell sha256sum $DL/val_libvulkan_mali.so | cut -d" " -f1); [ "$G" = "$VAL_SO_SHA256" ] || { echo "INVALID: driver on the phone sha256 ${G:0:12} != job ${VAL_SO_SHA256:0:12}" >&2; exit 77; }; fi
for t in t_vk_draw t_vk_compute t_vk_meta t_vk_feat; do timeout 100 adb -s $S push $V/out/a64-glibc/$t $DL/val_$t >/dev/null; done
for f in $V/shader/*.spv; do timeout 100 adb -s $S push $f $DL/val_$(basename $f) >/dev/null; done
echo "$1" >$HOME/.cache/mali-tmp/val_env.txt; echo "${2:-20}" >$HOME/.cache/mali-tmp/val_loop.txt; echo "${3:-}" >$HOME/.cache/mali-tmp/val_only.txt
adb -s $S push $HOME/.cache/mali-tmp/val_env.txt $DL/val_env.txt >/dev/null; adb -s $S push $HOME/.cache/mali-tmp/val_loop.txt $DL/val_loop.txt >/dev/null; adb -s $S push $HOME/.cache/mali-tmp/val_only.txt $DL/val_only.txt >/dev/null
# quick.py reads CMF_SERIAL/POCO_SERIAL only (farmd sets neither): hand it the serial we already resolved
case $PH in cmf) export CMF_SERIAL=${CMF_SERIAL:-$S};; poco) export POCO_SERIAL=${POCO_SERIAL:-$S};; esac
cd ~/Projects/mali-gaming; M=$(mktemp); trap 'rm -f "$M"' EXIT
# qdrv writes to the runs/quick of the repo scripts/quick points at (MediaDeck), not necessarily of cwd; QUICK_RUNS_DIR overrides
if [ -n "${QUICK_RUNS_DIR:-}" ]; then RD=$QUICK_RUNS_DIR; else RD=$(readlink -f scripts/quick); RD=${RD%/scripts/quick}/runs/quick; fi
timeout 280 scripts/quick/qdrv $PH $V/tests/valguest.sh --keep 2>&1 | tail -1 | tee /dev/stderr >/dev/null
# THIS run's result only: this phone's file written after we started. `ls -t | head -1` over every phone returned an older job's (or another phone's)
# result whenever qdrv failed before writing one (e.g. "session not READY"), so a smoke could fail with a foreign job's test line
R=$(find "$RD" -maxdepth 1 -name "????????-??????-$PH-drv-valguest.sh.jsonl" -newer "$M" | sort | tail -1)
[ -n "$R" ] || { echo "NO RESULT: qdrv wrote no $PH valguest result for this run (phone session not ready?); see the qdrv line above" >&2; exit 74; }
cat "$R"
