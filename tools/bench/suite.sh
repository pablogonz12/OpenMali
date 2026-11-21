#!/bin/bash
# SPDX-License-Identifier: MIT
# Uncapped, balanced benchmark suite: one GPU-heavy workload, one CPU-heavy workload and one
# named app workload, all with no vsync and no frame limiter. Every run writes one JSON line
# (schema mali-bench/1) with metric "render_fps" and, separately, displayed_fps only when a
# presentation path exists.
#
#   tools/bench/suite.sh [--seconds N] [--reps N] [--workloads gpu,cpu,app]
#                        [--out FILE] [--bin-dir DIR] [--spv-dir DIR] [--icd FILE]
#                        [--panel-hz N] [--app-cmd CMD] [--app-name NAME] [--json-only]
#
# Defaults: 15 s per run, one rep, workloads gpu,cpu,app, stdout plus the --out file.
# The tests are normal Vulkan clients: the ICD is whatever --icd sets (it is exported as
# VK_DRIVER_FILES/VK_ICD_FILENAMES), otherwise the environment's own ICD is used.
#
# Env: GPU_SIZE=1920x1080 GPU_SAMPLES=1 GPU_LAYERS=1 CPU_DRAWS=50000 APP_CMD=... APP_NAME=...
#      MALI_PANEL_HZ=120 MALI_UTIL=<file> MALI_TEMP_ZONE=<dir or temp file>
#      BENCH_BIN_DIR=... BENCH_DRIVER=... BENCH_DISPLAYED_FPS=... BENCH_ALLOW_PRESENTED=0
# Mali sysfs is read when present; otherwise gpu_busy_pct / temp are null (printed as n/a).
set -uo pipefail

D=$(cd "$(dirname "$0")/../.." && pwd)
SECS=15
REPS=1
WORKLOADS=gpu,cpu,app
OUT=
BIN_DIR=${BENCH_BIN_DIR:-$D/vk/out/${MALI_TARGET:-native}}
SPV_DIR=$D/vk/shader
ICD=
PANEL_HZ=${MALI_PANEL_HZ:-}
APP_CMD=${APP_CMD:-}
APP_NAME=${APP_NAME:-app}
JSON_ONLY=0

usage() {
   sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
   exit 2
}

while [ $# -gt 0 ]; do
   case $1 in
      --seconds) SECS=${2:?}; shift 2 ;;
      --reps) REPS=${2:?}; shift 2 ;;
      --workloads) WORKLOADS=${2:?}; shift 2 ;;
      --out) OUT=${2:?}; shift 2 ;;
      --bin-dir) BIN_DIR=${2:?}; shift 2 ;;
      --spv-dir) SPV_DIR=${2:?}; shift 2 ;;
      --icd) ICD=${2:?}; shift 2 ;;
      --panel-hz) PANEL_HZ=${2:?}; shift 2 ;;
      --app-cmd) APP_CMD=${2:?}; shift 2 ;;
      --app-name) APP_NAME=${2:?}; shift 2 ;;
      --json-only) JSON_ONLY=1; shift ;;
      -h|--help) usage ;;
      *) echo "unknown argument: $1" >&2; usage ;;
   esac
done

case $SECS in ''|*[!0-9]*) echo "--seconds must be an integer" >&2; exit 2 ;; esac
case $REPS in ''|*[!0-9]*) echo "--reps must be an integer" >&2; exit 2 ;; esac
[ "$SECS" -ge 1 ] && [ "$REPS" -ge 1 ] || { echo "--seconds and --reps must be >= 1" >&2; exit 2; }

if [ -n "${ICD:-}" ]; then
   [ -f "$ICD" ] || { echo "no ICD manifest at $ICD" >&2; exit 2; }
   export VK_DRIVER_FILES=$ICD VK_ICD_FILENAMES=$ICD
fi
export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE:-~all~}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/bench-suite.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

# ---------------------------------------------------------------- Mali sysfs (absent = null)
UTIL_FILE=
find_util() {
   local c
   [ -n "${MALI_UTIL:-}" ] && { UTIL_FILE=$MALI_UTIL; return; }
   for c in /sys/class/misc/mali0/device/utilisation /sys/class/misc/mali0/device/gpu_busy \
            /sys/class/misc/mali0/device/gpu_busy_percent /sys/kernel/debug/mali0/utilisation; do
      [ -r "$c" ] && { UTIL_FILE=$c; return; }
   done
   for c in /sys/class/devfreq/*/gpu_busy_percent; do
      [ -r "$c" ] && { UTIL_FILE=$c; return; }
   done
}
find_util

read_util() { # percent, empty when unavailable or not a number
   [ -n "$UTIL_FILE" ] && [ -r "$UTIL_FILE" ] || return 1
   local v
   v=$(tr -dc '0-9.' <"$UTIL_FILE" 2>/dev/null | head -c 16)
   [ -n "$v" ] || return 1
   awk -v v="$v" 'BEGIN { if (v+0 < 0 || v+0 > 1000) exit 1; print (v+0 > 100 ? (v+0)/10 : v+0) }'
}

TEMP_FILE=
find_temp() {
   local z t
   if [ -n "${MALI_TEMP_ZONE:-}" ]; then
      if [ -d "$MALI_TEMP_ZONE" ]; then TEMP_FILE=$MALI_TEMP_ZONE/temp; else TEMP_FILE=$MALI_TEMP_ZONE; fi
      return
   fi
   for z in /sys/class/thermal/thermal_zone*; do
      [ -r "$z/temp" ] || continue
      t=$(cat "$z/type" 2>/dev/null)
      case $t in *gpu*|*GPU*|*soc*|*SOC*|*mtktsgpu*) TEMP_FILE=$z/temp; return ;; esac
   done
}
find_temp

read_temp() {
   [ -n "$TEMP_FILE" ] && [ -r "$TEMP_FILE" ] || { echo ""; return; }
   local v
   v=$(tr -dc '0-9.-' <"$TEMP_FILE" 2>/dev/null | head -c 16)
   [ -n "$v" ] || { echo ""; return; }
   awk -v v="$v" 'BEGIN { if (v+0 > 1000) v/=1000; if (v+0 < -40 || v+0 > 200) exit 1; printf "%.1f", v+0 }' 2>/dev/null
}

cpu_idle_total() { # "idle jiffies" and "total jiffies"
   awk '/^cpu / { idle=$5+$6; total=0; for (i=2;i<=NF;i++) total+=$i; print idle, total; exit }' /proc/stat
}

host_name() {
   if [ -n "${BENCH_HOST:-}" ]; then echo "$BENCH_HOST"; return; fi
   if [ -r /proc/device-tree/model ]; then tr -d '\0' </proc/device-tree/model; return; fi
   hostname 2>/dev/null || echo unknown
}

# ---------------------------------------------------------------- one run
# run_one <workload> <class> <rep> <cmd...>
run_one() {
   local wl=$1 class=$2 rep=$3; shift 3
   local log=$TMP/run.log util=$TMP/util.txt temp_b temp_a idle0_t= idle0_tot= idle1_t= idle1_tot=
   : >"$util"
   temp_b=$(read_temp)
   read -r idle0_t idle0_tot < <(cpu_idle_total)

   [ "$JSON_ONLY" = 1 ] || printf '%s: %s (%s s) ... ' "$wl" "$*" "$SECS" >&2
   rm -f "$TMP/stop"
   (
      while [ ! -e "$TMP/stop" ]; do
         u=$(read_util) && printf '%s\n' "$u" >>"$util"
         sleep 0.2
      done
   ) &
   local sampler=$!
   "$@" >"$log" 2>&1
   local rc=$?
   : >"$TMP/stop"
   wait "$sampler" 2>/dev/null
   temp_a=$(read_temp)
   read -r idle1_t idle1_tot < <(cpu_idle_total)

   local render_fps frames window_s resolution samples draws cpu_ms present bound
   kv() { sed -n "s/^$1=//p" "$log" | tail -1; }
   render_fps=$(kv render_fps)
   frames=$(kv frames)
   window_s=$(kv seconds)
   resolution=$(kv resolution)
   samples=$(kv samples)
   draws=$(kv draws_per_frame)
   cpu_ms=$(kv cpu_ms_per_frame)
   present=$(kv present_path)
   bound=$(kv bound)

   # App workloads may be a JSON-producing binary (mdbench). Accept its "fps" only if the
   # command was explicitly an app workload and wrote a JSON file via APP_JSON.
   if [ -z "$render_fps" ] && [ "$class" = app ] && [ -n "${APP_JSON:-}" ] && [ -r "$APP_JSON" ]; then
      eval "$(python3 - "$APP_JSON" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    d = d.get("mdbench", d)
except Exception:
    sys.exit(1)
print("render_fps=%s" % d.get("fps", ""))
print("frames=%s" % d.get("frames_measured", d.get("frames_total", "")))
print("seconds=%s" % d.get("duration_s", ""))
if d.get("width") and d.get("height"):
    print("resolution=%sx%s" % (d["width"], d["height"]))
PY
)"
   fi

   if [ -z "$present" ]; then
      if [ "$class" = app ] && [ "${APP_HEADLESS:-0}" != 1 ]; then present=unknown; else present=none; fi
   fi
   [ -n "${seconds:-}" ] && window_s=$seconds
   # A missing duration is derived from the measured interval the fps refers to, never left empty: window_s and
   # frames/fps must describe the same measurement.
   if [ -z "$window_s" ] && [ -n "${frames:-}" ] && [ -n "${render_fps:-}" ]; then
      window_s=$(python3 -c 'import sys; f=float(sys.argv[1]); r=float(sys.argv[2]); print("%.3f" % (f/r) if r > 0 and f > 0 else "")' "$frames" "$render_fps")
   fi
   [ -n "$window_s" ] || window_s=$(kv seconds)
   [ -n "$resolution" ] || resolution=$(kv resolution)

   local gpu_busy=null temp_bs=null temp_as=null
   if [ -s "$util" ]; then
      gpu_busy=$(awk '{ s+=$1; n++ } END { if (n) printf "%.1f", s/n; else print "null" }' "$util")
   fi
   [ -n "$temp_b" ] && temp_bs=$temp_b
   [ -n "$temp_a" ] && temp_as=$temp_a

   local idle=null
   if [ -n "${idle0_tot:-}" ] && [ -n "${idle1_tot:-}" ] && [ "$idle1_tot" -gt "$idle0_tot" ] 2>/dev/null; then
      idle=$(awk -v i0="$idle0_t" -v t0="$idle0_tot" -v i1="$idle1_t" -v t1="$idle1_tot" \
         'BEGIN { printf "%.1f", (i1-i0)*100.0/(t1-t0) }')
   fi

   local headroom=null
   case ${PANEL_HZ:-} in
      ''|*[!0-9.]*) ;;
      *) [ -n "$render_fps" ] && headroom=$(awk -v f="$render_fps" -v h="$PANEL_HZ" 'BEGIN { if (h+0 > 0) printf "%.3f", f/h }') ;;
   esac

   local valid=true reason=
   if [ $rc -ne 0 ]; then valid=false; reason="exit-$rc"; fi
   if [ -z "$render_fps" ]; then valid=false; reason=${reason:-no-render-fps}; fi
   if [ "$present" != none ] && [ "$present" != unknown ] && [ "${BENCH_ALLOW_PRESENTED:-0}" != 1 ]; then
      valid=false; reason=${reason:-presentation-path}
   fi

   local displayed=${BENCH_DISPLAYED_FPS:-}
   local icd_now=${VK_DRIVER_FILES:-${VK_ICD_FILENAMES:-default}}

   KV_SCHEMA=mali-bench/1 KV_HOST="$(host_name)" KV_WORKLOAD="$wl" KV_CLASS="$class" \
   KV_REP="$rep" KV_FPS="$render_fps" KV_FRAMES="$frames" KV_WINDOW="$window_s" \
   KV_IDLE="$idle" KV_GBUSY="$gpu_busy" KV_TB="$temp_bs" KV_TA="$temp_as" \
   KV_PANEL="${PANEL_HZ:-}" KV_HEADROOM="$headroom" KV_RES="$resolution" KV_SAMPLES="$samples" \
   KV_DRAWS="$draws" KV_CPUMS="$cpu_ms" KV_BOUND="$bound" KV_DISPLAYED="$displayed" \
   KV_PRESENT="$present" KV_ICD="$icd_now" KV_DRIVER="${BENCH_DRIVER:-unknown}" \
   KV_VALID="$valid" KV_REASON="$reason" KV_CMD="$*" \
   python3 - "$OUT" <<'PY'
import json, os, sys
out = sys.argv[1]
e = os.environ
def num(k, cast=float):
    v = e.get(k, "")
    if v in ("", "null", "n/a"):
        return None
    try:
        return cast(v)
    except Exception:
        return None
row = {
    "schema": e["KV_SCHEMA"],
    "ts": __import__("datetime").datetime.now().astimezone().isoformat(timespec="seconds"),
    "host": e["KV_HOST"],
    "workload": e["KV_WORKLOAD"],
    "workload_class": e["KV_CLASS"],
    "rep": int(e["KV_REP"]),
    "metric": "render_fps",
    "render_fps": num("KV_FPS"),
    "frames": num("KV_FRAMES", int),
    "window_s": num("KV_WINDOW"),
    "cpu_idle_pct": num("KV_IDLE"),
    "gpu_busy_pct": None if e.get("KV_GBUSY") == "null" else num("KV_GBUSY"),
    "temp_before_c": None if e.get("KV_TB") == "null" else num("KV_TB"),
    "temp_after_c": None if e.get("KV_TA") == "null" else num("KV_TA"),
    "panel_hz": num("KV_PANEL"),
    "headroom": num("KV_HEADROOM"),
    "resolution": e.get("KV_RES") or None,
    "samples": num("KV_SAMPLES", int),
    "draws_per_frame": num("KV_DRAWS", int),
    "cpu_ms_per_frame": num("KV_CPUMS"),
    "bound": e.get("KV_BOUND") or None,
    "present_path": e["KV_PRESENT"],
    "icd": e["KV_ICD"],
    "driver": e["KV_DRIVER"],
    "valid": e["KV_VALID"] == "true",
    "reason": e.get("KV_REASON") or None,
    "cmd": e["KV_CMD"],
}
# displayed_fps is a separate quantity and is emitted ONLY when a presentation path exists.
if e.get("KV_PRESENT", "none") != "none" and e.get("KV_DISPLAYED"):
    row["displayed_fps"] = num("KV_DISPLAYED")
line = json.dumps(row, separators=(",", ":"))
print(line)
if out:
    with open(out, "a") as f:
        f.write(line + "\n")
PY
   [ "$JSON_ONLY" = 1 ] || {
      printf 'render_fps=%s frames=%s cpu_ms=%s gpu_busy=%s temp=%s->%s %s\n' \
         "${render_fps:-n/a}" "${frames:-n/a}" "${cpu_ms:-n/a}" \
         "$([ "$gpu_busy" = null ] && echo n/a || echo "$gpu_busy")" \
         "$([ "$temp_bs" = null ] && echo n/a || echo "$temp_bs")" \
         "$([ "$temp_as" = null ] && echo n/a || echo "$temp_as")" \
         "$([ "$valid" = true ] && echo ok || echo "INVALID($reason)")" >&2
   }
}

gpu_cmd() {
   local w h s l
   w=${GPU_SIZE:-1920x1080}; w=${w%x*}; h=${GPU_SIZE:-1920x1080}; h=${h#*x}
   printf '%s\n' "$BIN_DIR/t_bench_gpu --seconds $SECS --width $w --height $h --samples ${GPU_SAMPLES:-1} --layers ${GPU_LAYERS:-1} --spv-dir $SPV_DIR"
}

cpu_cmd() {
   printf '%s\n' "$BIN_DIR/t_bench_cpu --seconds $SECS --draws ${CPU_DRAWS:-50000} --spv-dir $SPV_DIR"
}

app_cmd() {
   if [ -n "$APP_CMD" ]; then
      printf '%s\n' "$APP_CMD"
      return
   fi
   local bin=$D/bench/mdbench/out/mdbench-$([ "$(uname -m)" = aarch64 ] && echo aarch64 || echo native)
   printf '%s\n' "$bin --scene cube --layers ${APP_LAYERS:-8} --iters ${APP_ITERS:-64} --wsi headless --size ${APP_SIZE:-1920x1080} --seconds $SECS --json $APP_JSON --label $APP_NAME"
}

for wl in ${WORKLOADS//,/ }; do
   case $wl in
      gpu|gpu-heavy) class=gpu;   cmd=$(gpu_cmd) ;;
      cpu|cpu-heavy) class=cpu;   cmd=$(cpu_cmd) ;;
      app|app-heavy|$APP_NAME)
         class=app
         if [ -n "$APP_CMD" ]; then APP_JSON=; APP_HEADLESS=0; else APP_JSON=$TMP/app.json; APP_HEADLESS=1; fi
         cmd=$(app_cmd) ;;
      *) echo "unknown workload: $wl (gpu, cpu, app)" >&2; exit 2 ;;
   esac
   set -- $cmd
   if [ "$class" != app ] || [ -z "$APP_CMD" ]; then
      [ -x "$1" ] || { echo "no benchmark binary: $1 (build it first: vk/build.sh, bench/mdbench/build.sh)" >&2; exit 2; }
   else
      command -v "$1" >/dev/null || [ -x "$1" ] || { echo "no app binary: $1" >&2; exit 2; }
   fi
   for rep in $(seq 1 "$REPS"); do
      run_one "$wl" "$class" "$rep" "$@"
   done
done
