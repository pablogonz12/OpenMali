#!/bin/bash
# SPDX-License-Identifier: MIT
# X11 WSI on a server WITHOUT DRI3 (STEAM-GAPS.md "X11 WSI"): the X4's Xwayland has no DRM node, hence no DRI3, and
# MIT-SHM / PutImage is the only present path there. zink asks vkGetPhysicalDeviceSurfaceSupportKHR before it builds
# anything and gives up when the answer is FALSE (zink_kopper.c:130-132), while wsi_common_x11 answers FALSE without
# DRI3 unless wsi_device->sw is already set (wsi_common_x11.c:713-719) - so the no-DRI3 switch has to be taken at the
# first surface query, not in vkCreateSwapchainKHR. This case fails on a driver that switches too late: support is
# FALSE and t_vk_xcb stops at line 179 ("surface not supported by the graphics queue").
#
# The X server is a second Xwayland started inside gamescope's headless session with XWAYLAND_NO_GLAMOR=1, which
# prints "Disabling glamor and dri3 support" and leaves DRI3 out of the extension list while MIT-SHM and Present stay
# - the X4's situation, which no Xvfb on a normal host reproduces (Xvfb is not installed, and Xwayland's own X server
# in the harness does have DRI3). The test asserts DRI3 really is absent, so it cannot silently prove nothing.
# MALI_WSI=hw runs the fake backend as if it were hardware (dma-buf first, per-surface fallback), MALI_SOFT=1 renders
# the clear on the CPU so the pixel the window shows is the pixel the driver wrote (the --draw path is not usable here:
# its frame record is rejected with VK_ERROR_DEVICE_LOST on this backend, and a clear alone proves the present path).
# Usage: xwsi-nodri3.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without gamescope/Xwayland/xdpyinfo/t_vk_xcb.
set -u
OUT=$1; GPU=${2:-v11}
T=$(cd "$(dirname "$0")" && pwd); V=$(cd "$T/.." && pwd)
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
for c in gamescope Xwayland xdpyinfo; do command -v $c >/dev/null || { echo "skipped: no $c"; exit 0; }; done
OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

SV=$V
cat >"$W/inner.sh" <<EOF
#!/bin/sh
# gamescope starts its own Xwayland on :1; a second one on :77, glamor off, gives the server without DRI3.
sock=\${GAMESCOPE_WAYLAND_DISPLAY:-}
[ -n "\$sock" ] || sock=\$(ls "\$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^gamescope-[0-9]+\$' | head -1)
[ -n "\$sock" ] || { echo "FAIL: no gamescope Wayland socket in \$XDG_RUNTIME_DIR"; echo 1 >"$W/rc"; exit 1; }
XWAYLAND_NO_GLAMOR=1 WAYLAND_DISPLAY="\$sock" Xwayland :77 -nolisten tcp -extension DRI3 >"$W/xw.log" 2>&1 &
i=0; while [ \$i -lt 60 ]; do xdpyinfo -display :77 >/dev/null 2>&1 && break; sleep 0.25; i=\$((i+1)); done
xdpyinfo -display :77 >"$W/xdpy.txt" 2>&1 || { echo "FAIL: no X server on :77"; tail -5 "$W/xw.log"; echo 1 >"$W/rc"; exit 1; }
if grep -qx '[[:space:]]*DRI3' "$W/xdpy.txt"; then
   echo "FAIL: the test X server has DRI3, so nothing is tested"; echo 1 >"$W/rc"; exit 1
fi
grep -qx '[[:space:]]*MIT-SHM' "$W/xdpy.txt" || { echo "FAIL: the test X server has no MIT-SHM"; echo 1 >"$W/rc"; exit 1; }
echo "x11-no-dri3: server on :77 has no DRI3, has MIT-SHM"
export DISPLAY=:77 MALI_BACKEND=fake MALI_FAKE_GPU=$GPU
export VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json
export MALI_WSI=hw MALI_SOFT=1 MALI_WSI_DEBUG=1
timeout 120 "$OUT/t_vk_xcb" --direct "$OUT/libvulkan_mali.so" --spv-dir "$SV/shader" --pixels >"$W/xcb.log" 2>&1
echo \$? >"$W/rc"
EOF
chmod +x "$W/inner.sh"
timeout 200 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
grep -vE 'alvr|CAP_SYS_NICE|Performance will|radv|xkbcomp|^>|^Errors from' "$W/gs.log" | grep -vE '^\[gamescope\]' | head -20
grep -E 'mali_vk: (wsi|surface|.*create_swapchain)' "$W/xcb.log" | head -6
grep -E 'present path|Presented' "$W/xcb.log" | head -3
rc=$(cat "$W/rc" 2>/dev/null || echo 1)
[ "$rc" = 0 ] || { echo "FAIL: t_vk_xcb exit $rc"; tail -15 "$W/xcb.log"; exit 1; }
echo "ok    x11 no-dri3 ($GPU): support TRUE, frame presented by MIT-SHM/PutImage"
