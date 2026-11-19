#!/bin/bash
# SPDX-License-Identifier: MIT
# tools/megabonk/run.sh <alias> [--wait 90] [--secs 60] [--no-gs] [--no-cull] [--no-descidx] [--farm] [--dry-run] [--out DIR]
# Megabonk (Steam appid 3405340) on OUR driver on one phone; prints fps only when the evidence says "ours". See README.md.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
APPID=${MB_APPID:-3405340}
QUICK_ROOT=${MB_QUICK_ROOT:-$HOME/Projects/mali-gaming}      # farm.toml quick_root: tools/droiddeckctl
STATE=${MALI_FARM_STATE:-$HOME/.local/state/mali-farm}
TEMP_MAX=${MB_TEMP_MAX:-38}
WAIT=90 SECS=60 DRY=0 FARM=0 GS=1 CULL=1 DESCIDX=1 ALIAS="" OUT=""
ARGS=("$@")
usage() { sed -n 3,4p "$0" | sed 's/^# \?//' >&2; exit 2; }
while [ $# -gt 0 ]; do
  case $1 in
    --wait) WAIT=${2:?}; shift 2 ;; --secs) SECS=${2:?}; shift 2 ;; --out) OUT=${2:?}; shift 2 ;;
    --no-gs) GS=0; shift ;; --no-cull) CULL=0; shift ;; --no-descidx) DESCIDX=0; shift ;;
    --farm) FARM=1; shift ;; --dry-run) DRY=1; shift ;; -h|--help) usage ;;
    -*) echo "unknown option $1" >&2; usage ;; *) ALIAS=$1; shift ;;
  esac
done
[ -n "$ALIAS" ] || usage
if [ -z "$OUT" ]; then   # live: private dir from mktemp (no predictable path); dry-run only prints a name
  if [ $DRY = 1 ]; then OUT=${TMPDIR:-/tmp}/megabonk-$ALIAS-DRYRUN; else OUT=$(mktemp -d "${TMPDIR:-/tmp}/megabonk-$ALIAS-XXXXXX") || exit 1; fi
fi
PY=${MB_PYTHON:-python3}

# every phone-touching command goes through run/cap: --dry-run prints it and does nothing
run() { if [ $DRY = 1 ]; then printf '+ %s\n' "$*"; else "$@"; fi; }
cap() { local f=$1; shift; if [ $DRY = 1 ]; then printf '+ %s > %s\n' "$*" "$f"; : > /dev/null; else "$@" > "$f" 2>&1; fi; }
say() { echo "[megabonk] $*"; }
must() { run "$@" || { say "FAILED (rc=$?): $*"; exit 7; }; }     # required step: stop here, the EXIT trap (once armed) still restores

# --farm: take a farm hold (queued fairly, restored by farmd on release) and re-run ourselves inside it
if [ $FARM = 1 ] && [ -z "${FARM_HOLD:-}" ]; then
  inner=(); for a in "${ARGS[@]}"; do [ "$a" = --farm ] || inner+=("$a"); done
  total=$(( WAIT + SECS + 240 )); mins=$(( (total + 59) / 60 )); [ $mins -gt 20 ] && mins=20
  run farm hold "$ALIAS" --max "$mins" -- "$HERE/run.sh" "${inner[@]}"
  exit $?
fi

if [ $DRY = 1 ]; then SERIAL="<serial-of-$ALIAS>"; else SERIAL=${ANDROID_SERIAL:-$("$HERE/../dev-serial" "$ALIAS")} || exit 1; fi
export ANDROID_SERIAL=$SERIAL ADB_SERIAL=$SERIAL PYTHONPATH=$QUICK_ROOT/tools
DCTL=("$PY" -I "$QUICK_ROOT/tools/droiddeckctl" --serial "$SERIAL")
GUEST=("${DCTL[@]}" guest --timeout 60 --)
[ $DRY = 1 ] || mkdir -p "$OUT" || exit 1
RESTORED=0
restore() {   # always: quit the game, stop the session, clear env, vkBridge=true (qrestore does all of it); drop debug/driver selection
  [ $RESTORED = 1 ] && return; RESTORED=1
  say "restore"
  run "${DCTL[@]}" quit "$APPID" --timeout 30
  run "${DCTL[@]}" guest --timeout 30 -- sh -c 'echo off > /root/.malideck/debug; echo bridge > /root/.malideck/driver'
  run "$QUICK_ROOT/scripts/quick/qrestore" "$ALIAS"
}

# (1) pre-flight --------------------------------------------------------------------------------------------------------------
say "(1) pre-flight $ALIAS"
if [ $DRY = 0 ]; then
  [ -e "$STATE/disabled/$ALIAS" ] && { say "REFUSED: $ALIAS is disabled by the owner"; exit 5; }
  cap "$OUT/thermal.json" "${DCTL[@]}" thermal
  t=$("$PY" -I -c 'import json,sys;print(json.load(open(sys.argv[1])).get("batteryC"))' "$OUT/thermal.json" 2>/dev/null)
  case $t in None|"") say "REFUSED: cannot read battery temperature"; exit 5 ;; esac
  "$PY" -I -c 'import sys;sys.exit(0 if float(sys.argv[1])<float(sys.argv[2]) else 1)' "$t" "$TEMP_MAX" ||
    { say "REFUSED: battery ${t} C >= ${TEMP_MAX} C (let it cool; do not override)"; exit 5; }
  say "thermal ${t} C < ${TEMP_MAX} C"
else
  printf '+ test ! -e %s/disabled/%s            # device enabled\n' "$STATE" "$ALIAS"
  printf '+ droiddeckctl thermal                # batteryC < %s\n' "$TEMP_MAX"
fi
# foreground_launch phones: farmd/qsafe own the session. Foreground must be the launcher/MediaDeck, never another app.
must "$QUICK_ROOT/scripts/quick/qpreflight" "$ALIAS"
# from here on the device is changed: arm the restore (a refused/disabled phone above is never touched)
trap 'restore' EXIT
trap 'exit 130' INT TERM
must "${DCTL[@]}" start steam --wait --reuse --timeout 240

# (2) select our driver, with evidence --------------------------------------------------------------------------------------
say "(2) driver ours (MediaDeck Debug on + Driver ours, MaliBridge off; bundle 'Mali-open*' must be imported by hand beforehand)"
must "${GUEST[@]}" sh -c 'echo on > /root/.malideck/debug; echo ours > /root/.malideck/driver'
must "${DCTL[@]}" prefs set --type bool session vkBridge false
collect_evidence() {   # -> $1 file of key=value evidence lines
  local f=$1
  if [ $DRY = 1 ]; then
    printf '+ collect evidence into %s: cat /root/.malideck/{driver,debug}; prefs get session (vkBridge); env list;\n' "$f"
    printf '+   adb shell cat driver-setup.txt | tail -3; guest vulkaninfo --summary | grep driverName; DXVK log "Found device"\n'; return
  fi
  {
    echo "driver_pref=$("${GUEST[@]}" cat /root/.malideck/driver 2>/dev/null | tr -d '\r\n ')"
    echo "debug_pref=$("${GUEST[@]}" cat /root/.malideck/debug 2>/dev/null | tr -d '\r\n ')"
    echo "vkBridge=$("${DCTL[@]}" prefs get session 2>/dev/null | "$PY" -I -c 'import json,sys;print(str(json.load(sys.stdin).get("vkBridge")).lower())' 2>/dev/null)"
    "${DCTL[@]}" env list 2>/dev/null | sed 's/^/env=/'
    adb -s "$SERIAL" shell "tail -3 /sdcard/Android/data/${DROIDDECK_PACKAGE:-com.ludashi.benchmark}/files/driver-setup.txt" 2>/dev/null | tr -d '\r'
    "${GUEST[@]}" sh -c 'vulkaninfo --summary 2>/dev/null | grep -m1 driverName' 2>/dev/null | tr -d '\r'
    [ -f "$OUT/dxvk.log" ] && grep -h "Found device" "$OUT/dxvk.log" 2>/dev/null
  } > "$f"
}
# (3) env via droiddeckctl (one-shot, next session; cleared again by restore) ---------------------------------------------------
say "(3) env"
ENVL=("MALI_SHADER_CACHE=${MB_SHADER_CACHE:-/root/mali-shader-cache}" DXVK_LOG_LEVEL=info DXVK_LOG_PATH=/tmp/dxvk-logs)
# advertise shims: GS = lane/gsshim #156, CULL = lane/cullshim #164, DESCIDX = #166 (a driver without them ignores the variable)
[ "$GS" = 1 ] && ENVL+=(MALI_ADVERTISE_GS=1)
[ "$CULL" = 1 ] && ENVL+=(MALI_ADVERTISE_CULL=1)
[ "$DESCIDX" = 1 ] && ENVL+=(MALI_ADVERTISE_DESCIDX=1)
must "${GUEST[@]}" mkdir -p "${MB_SHADER_CACHE:-/root/mali-shader-cache}" /tmp/dxvk-logs
run "${GUEST[@]}" sh -c 'rm -f /tmp/dxvk-logs/*.log'     # stale logs would feed old "Found device" lines to the evidence gate
must "${DCTL[@]}" env set "${ENVL[@]}"
# a running session keeps its old env: restart so the env and the driver selection apply
must "${DCTL[@]}" stop
must "${DCTL[@]}" start steam --wait --timeout 240

# pre-launch gate on the RESTARTED session (the old one still runs the old driver): refuse before the game starts
collect_evidence "$OUT/evidence-pre.txt"
if [ $DRY = 1 ]; then printf '+ %s %s/mblib.py evidence %s/evidence-pre.txt --allow-unknown   # exit != 0 (bridge/panvk/unknown/conflict) = REFUSE, no launch\n' "$PY" "$HERE" "$OUT"
else
  "$PY" -I "$HERE/mblib.py" evidence "$OUT/evidence-pre.txt" --allow-unknown || { say "REFUSED before launch: fix the driver selection (Debug on + Driver ours + bundle imported)"; exit 3; }
fi

# (4) launch, steady scene -----------------------------------------------------------------------------------------------------
say "(4) launch $APPID, wait ${WAIT}s"
run "${DCTL[@]}" launch "$APPID" --wait --timeout 300
rc=$?
[ $DRY = 1 ] && printf '+ sleep %s\n' "$WAIT" || sleep "$WAIT"
if [ $DRY = 0 ]; then
  "${DCTL[@]}" guest --timeout 30 -- sh -c 'cat /tmp/dxvk-logs/*.log 2>/dev/null' > "$OUT/dxvk.log" 2>&1
  "${DCTL[@]}" logs latest "$OUT/session-logs" > /dev/null 2>&1
  cat "$OUT"/session-logs/* 2>/dev/null | cat >> "$OUT/dxvk.log" 2>/dev/null
else printf '+ droiddeckctl guest cat /tmp/dxvk-logs/*.log > %s/dxvk.log; droiddeckctl logs latest %s/session-logs\n' "$OUT" "$OUT"; fi
focus=1; [ $DRY = 1 ] || focus=$("${DCTL[@]}" state 2>/dev/null | grep -c "\"focusedApp\": $APPID")
if [ $DRY = 0 ] && [ "$focus" = 0 ]; then
  say "FAILED: Megabonk is not running/focused (launch rc=$rc). First DXVK/Vulkan error lines:"
  "$PY" -I "$HERE/mblib.py" errors "$OUT/dxvk.log" 12
  collect_evidence "$OUT/evidence-post.txt"; "$PY" -I "$HERE/mblib.py" evidence "$OUT/evidence-post.txt"
  exit 6
fi

# (5) sample 60 s ----------------------------------------------------------------------------------------------------------------
say "(5) pacing ${SECS}s (SurfaceFlinger present-to-present histogram of the game layer; the same source as bench.sh/qgame)"
cap "$OUT/pacing.json" "${DCTL[@]}" pacing --seconds "$SECS"
cap "$OUT/thermal-post.json" "${DCTL[@]}" thermal
collect_evidence "$OUT/evidence.txt"
if [ $DRY = 1 ]; then
  printf '+ %s %s/mblib.py evidence %s/evidence.txt   # refuses (exit 3) unless kind=ours\n' "$PY" "$HERE" "$OUT"
  printf '+ %s %s/mblib.py stats %s/pacing.json %s   # avg fps, 1%% low, %% frames <= 8.3 ms\n' "$PY" "$HERE" "$OUT" "$SECS"
  printf '+ %s %s/mblib.py errors %s/dxvk.log 12   # printed only if no frames\n' "$PY" "$HERE" "$OUT"
  exit 0
fi
if ! "$PY" -I "$HERE/mblib.py" evidence "$OUT/evidence.txt"; then say "no fps reported (evidence refused)"; exit 3; fi
"$PY" -I "$HERE/mblib.py" stats "$OUT/pacing.json" "$SECS"; src=$?
if [ $src != 0 ]; then say "no frames; first DXVK/Vulkan error lines:"; "$PY" -I "$HERE/mblib.py" errors "$OUT/dxvk.log" 12; fi
say "artifacts: $OUT"
exit $src
