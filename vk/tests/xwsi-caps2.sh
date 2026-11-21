#!/bin/bash
# SPDX-License-Identifier: MIT
# The *2KHR surface queries must answer what the non-2 forms answer (CR #323 N5). They are answered by wsi_common
# itself (wsi_common.c:1149/1244) and read the device's wsi_device, so they ran outside the per-surface scope; on the
# X11 software path the two families must not disagree. This case asserts that on a server with no DRI3 (where the
# 1-form answers the software story) and with MALI_WSI=debug the driver is on the software path.
# Usage: xwsi-caps2.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
set -u
OUT=$1; GPU=${2:-v10}
T=$(cd "$(dirname "$0")" && pwd); V=$(cd "$T/.." && pwd)
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
for c in gamescope Xwayland xdpyinfo; do command -v $c >/dev/null || { echo "skipped: no $c"; exit 0; }; done
OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cat >"$W/inner.sh" <<INNER
#!/bin/sh
sock=\${GAMESCOPE_WAYLAND_DISPLAY:-}
[ -n "\$sock" ] || sock=\$(ls "\$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^gamescope-[0-9]+\$' | head -1)
[ -n "\$sock" ] && export WAYLAND_DISPLAY="\$sock"
XWAYLAND_NO_GLAMOR=1 Xwayland :77 -nolisten tcp -extension DRI3 >"$W/xw.log" 2>&1 &
i=0; while [ \$i -lt 60 ]; do xdpyinfo -display :77 >/dev/null 2>&1 && break; sleep 0.25; i=\$((i+1)); done
xdpyinfo -display :77 >"$W/xdpy.txt" 2>&1 || { echo "FAIL: no X server on :77"; echo 1 >"$W/rc"; exit 1; }
grep -qx '[[:space:]]*DRI3' "$W/xdpy.txt" && { echo "FAIL: :77 has DRI3"; echo 1 >"$W/rc"; exit 1; }
export DISPLAY=:77 MALI_BACKEND=fake MALI_FAKE_GPU=$GPU
export VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json
export MALI_WSI=hw MALI_WSI_DEBUG=1   # hw: the software decision must come from the no-DRI3 probe, not from the fake backend
timeout 90 "$OUT/t_vk_xcb" --direct "$OUT/libvulkan_mali.so" --caps2 >"$W/xcb.log" 2>&1
echo \$? >"$W/rc"
INNER
chmod +x "$W/inner.sh"
timeout 200 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
rc=$(cat "$W/rc" 2>/dev/null || echo 1)
if [ "$rc" = 77 ]; then echo "skipped: $(grep -m1 skipped "$W/xcb.log")"; exit 0; fi
grep -E "^(surface support|caps|caps2|formats)" "$W/xcb.log"
[ "$rc" = 0 ] || { echo "FAIL: t_vk_xcb --caps2 exit $rc"; tail -8 "$W/xcb.log"; exit 1; }
grep -q "software (PutImage)" "$W/xcb.log" || { echo "FAIL: the no-DRI3 server did not take the software path"; grep "present path" "$W/xcb.log" | head -2; exit 1; }
echo "ok    caps2 ($GPU): the *2KHR surface queries agree with the 1-forms on the software path"
