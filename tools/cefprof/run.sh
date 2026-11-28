#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host-only: what does Chromium (ANGLE Vulkan) ask of our driver per displayed frame? Runs Chromium on a private
# headless sway + Xwayland (never on the user's desktop) against the host build of the driver with a fake GPU, under
# the cefprof layer, on carousel.html. Pixels are meaningless (nothing executes). Not wired into check.sh.
#   tools/cefprof/build.sh && tools/cefprof/run.sh [options] [-- extra chromium flags]
#   --gpu v10|v11|v13   fake GPU (X4-like | CMF-like | X8-like; default v13)
#   --backend fake|kbfake   fake (default) = the driver's fake backend. kbfake = product kbase backend on a fake kernel
#                           with ioctls counted; KNOWN ISSUE: the GPU process segfaults in the test kernel (README.md)
#   --mode steam|composite  page variant      --in-process    add --in-process-gpu
#   --frames N (300)    --skip N (180)        --out DIR       --label NAME
#   --no-layer          control run without the layer
# CHROME=/path/to/chrome (default: Chrome for Testing 126.0.6478.183, the version of Steam's CEF).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
gpu=v13 backend=fake mode=steam inproc=0 frames=300 skip=180 out= label= layer=1
while [ $# -gt 0 ]; do
   case $1 in
      --gpu) gpu=$2; shift 2 ;;
      --backend) backend=$2; shift 2 ;;
      --mode) mode=$2; shift 2 ;;
      --in-process) inproc=1; shift ;;
      --frames) frames=$2; shift 2 ;;
      --skip) skip=$2; shift 2 ;;
      --out) out=$2; shift 2 ;;
      --label) label=$2; shift 2 ;;
      --no-layer) layer=0; shift ;;
      --) shift; break ;;
      *) echo "unknown option $1" >&2; exit 2 ;;
   esac
done
CHROME=${CHROME:-$HOME/.cache/mali-orchestrator/host-dumps/chrome-126/chrome-linux64/chrome}
[ -x "$CHROME" ] || { echo "no Chromium at $CHROME (set CHROME; Chrome for Testing 126.0.6478.183 linux64 matches Steam)" >&2; exit 2; }
case $backend in
   fake) icd=$root/vk/out/native/mali_icd.json ;;
   kbfake) icd=$here/out/tree/vk/out/native/mali_icd.json ;;
   *) echo "bad --backend" >&2; exit 2 ;;
esac
[ -f "$icd" ] && [ -f "$here/out/layer/cefprof.json" ] || { echo "run $here/build.sh first" >&2; exit 2; }
label=${label:-$gpu-$backend-$mode$([ $inproc = 1 ] && echo -inproc)}
out=${out:-$here/out/runs}/$label
rm -rf -- "${out:?}"; mkdir -p "$out"

run=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/cefprof.XXXXXX"); chmod 700 "$run"
profile=$(mktemp -d)
swaypid= chromepid=
cleanup() {
   [ -n "$chromepid" ] && { kill -- "-$chromepid" 2>/dev/null; sleep 0.5; kill -9 -- "-$chromepid" 2>/dev/null; }
   [ -n "$swaypid" ] && { kill "$swaypid" 2>/dev/null; sleep 0.3; }
   rm -rf -- "$run" "$profile"
}
trap cleanup EXIT
printf "output HEADLESS-1 resolution 1920x1080\nxwayland force\ndefault_border none\nexec sh -c 'echo \$DISPLAY > %s/display'\n" "$run" > "$run/sway.cfg"
env -u DISPLAY -u WAYLAND_DISPLAY -u SWAYSOCK XDG_RUNTIME_DIR="$run" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
   setsid sway -c "$run/sway.cfg" > "$out/sway.log" 2>&1 &
swaypid=$!
for _ in $(seq 50); do [ -s "$run/display" ] && break; sleep 0.2; done
[ -s "$run/display" ] || { echo "nested sway did not start (see $out/sway.log)" >&2; exit 1; }

env_=(DISPLAY="$(cat "$run/display")" XDG_RUNTIME_DIR="$run" DBUS_SESSION_BUS_ADDRESS=disabled:
   VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES="$icd" CEFPROF_OUT="$out/prof" CEFPROF_KSTATS="$out/kstats.bin"
   CEFPROF_SKIP="$skip" CEFPROF_FRAMES="$frames" MALI_WSI_DEBUG=1)
if [ "$backend" = fake ]; then
   env_+=(MALI_BACKEND=fake MALI_FAKE_GPU="$gpu")
else
   env_+=(KB_FAKE_ARCH="${gpu#v}")
fi
[ $layer = 1 ] && env_+=(VK_ADD_LAYER_PATH="$here/out/layer" VK_LAYER_PATH="$here/out/layer" VK_INSTANCE_LAYERS=VK_LAYER_MALI_cefprof)
flags=(--user-data-dir="$profile" --no-first-run --no-default-browser-check --disable-background-networking --disable-component-update
   --ozone-platform=x11 --use-gl=angle --use-angle=vulkan --enable-gpu-rasterization --ignore-gpu-blocklist --no-sandbox --disable-gpu-sandbox
   --disable-gpu-watchdog --window-size=1920,1080 --kiosk --allow-file-access-from-files --enable-logging=stderr --remote-debugging-port=0)
[ $inproc = 1 ] && flags+=(--in-process-gpu)
flags+=("$@")
{ "$CHROME" --version; echo "commit $(git -C "$root" rev-parse --short=10 HEAD)"; echo "icd $icd"; sha256sum "${icd%mali_icd.json}libvulkan_mali.so";
  echo "env ${env_[*]}"; echo "flags ${flags[*]}"; } > "$out/run.txt"

env -u WAYLAND_DISPLAY "${env_[@]}" setsid "$CHROME" "${flags[@]}" "file://$here/carousel.html?mode=$mode" > "$out/chrome.log" 2>&1 &
chromepid=$!
t0=$SECONDS
while [ $((SECONDS - t0)) -lt "${CEFPROF_TIMEOUT:-90}" ]; do
   [ -f "$out/prof.done" ] && break
   [ $layer = 0 ] && [ $((SECONDS - t0)) -ge 15 ] && break
   kill -0 "$chromepid" 2>/dev/null || break
   sleep 1
done
timeout 20 python3 -I "$here/cdp.py" "$profile" gpuinfo > "$out/gpuinfo.json" 2>&1
timeout 20 python3 -I "$here/cdp.py" "$profile" eval 'window.frames_drawn' carousel > "$out/rafs.txt" 2>&1
grep -hE '"(glRenderer|glVersion|glImplementationParts|gpu_compositing|rasterization)"' "$out/gpuinfo.json" | sed 's/^ */  /'
echo "  rAF callbacks so far: $(cat "$out/rafs.txt")"
grep -E "BAD_CONFIG|device lost|DEVICE_LOST|mali_cmd: unsupported|exit_code|GPU process" "$out/chrome.log" | grep -oE "(ERROR|WARNING|mali_cmd).*" | sort | uniq -c | head -8
if [ $layer = 1 ]; then
   [ -f "$out/prof.done" ] || { echo "no report: fewer than $((skip + frames)) presents (see $out/chrome.log)" >&2; exit 1; }
   python3 -I "$here/report.py" "$out" > "$out/report.txt" && echo "report: $out/report.txt"
fi
