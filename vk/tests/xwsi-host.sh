#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test of the X11 WSI (STEAM-GAPS.md "X11 WSI"), no hardware: an X server from `gamescope --backend headless` (its Xwayland has
# DRI3/Present/MIT-SHM; the host has no Xvfb), our ICD on the fake backend (software present path: shm/PutImage), and
#   1. t_vk_xcb: xcb window -> VK_KHR_xcb_surface -> swapchain -> acquire -> clear -> present x3
#   2. glxinfo -B with zink on our ICD: GLX needs kopper, i.e. an xcb surface + swapchain, so a GL version proves the whole chain
# Usage: xwsi-host.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without gamescope/Xwayland, without t_vk_xcb, or when the ICD lacks VK_KHR_xcb_surface (Mesa tree without x11).
set -u
OUT=$1; GPU=${2:-v10}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
command -v gamescope >/dev/null && command -v Xwayland >/dev/null || { echo "skipped: no gamescope/Xwayland for a headless X server"; exit 0; }
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
cat >"$W/inner.sh" <<EOF
#!/bin/sh
# inside gamescope: DISPLAY is its Xwayland. The gamescope WSI layer is off: it would turn the X11 swapchains into Wayland ones, which
# the fake backend cannot export, and it is not what is under test.
export ENABLE_GAMESCOPE_WSI=0 MALI_BACKEND=fake MALI_FAKE_GPU=$GPU VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json
timeout 60 "$OUT/t_vk_xcb" --expect-name Mali >"$W/xcb.log" 2>&1; echo \$? >"$W/xcb.rc"
# X11 default (STEAM-GAPS.md "X11 WSI"): an xcb surface gets the software PutImage path, and pretending the server has
# no DRI3 (MALI_WSI_FORCE_NO_DRI3) must not change that. Wayland clients never run this code (no X connection is made
# before an X11 surface exists).
export MALI_WSI_FORCE_NO_DRI3=1 MALI_WSI_DEBUG=1
timeout 60 "$OUT/t_vk_xcb" --expect-name Mali >"$W/nodri3.log" 2>&1; echo \$? >"$W/nodri3.rc"
unset MALI_WSI_FORCE_NO_DRI3
if command -v glxinfo >/dev/null; then
   MESA_LOADER_DRIVER_OVERRIDE=zink LIBGL_KOPPER_DRI2=1 timeout 60 glxinfo -B >"$W/glx.log" 2>&1; echo \$? >"$W/glx.rc"
else echo skipped >"$W/glx.rc"; fi
EOF
chmod +x "$W/inner.sh"
timeout 150 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
[ -f "$W/xcb.rc" ] || { echo "FAIL: the test did not run inside gamescope"; tail -20 "$W/gs.log"; exit 1; }
grep -v alvr "$W/xcb.log"
grep -E "present path|create_swapchain:" "$W/nodri3.log" | head -2
[ "$(cat "$W/nodri3.rc")" = 0 ] && grep -q '^PASS' "$W/nodri3.log" && grep -q "present path: shm=0 dri3=0 (software)" "$W/nodri3.log" \
   || { echo "FAIL: X11 software present default"; tail -5 "$W/nodri3.log"; exit 1; }
# Capability rule: this Xwayland has MIT-SHM but the ICD has no EXT_external_memory_host, so the present must be
# PutImage (no SHM image, no buffer blit) and must report VK_SUCCESS, never VK_ERROR_FEATURE_NOT_PRESENT.
grep -q "x11 present: putimage" "$W/nodri3.log" || { echo "FAIL: present did not report the putimage branch"; tail -5 "$W/nodri3.log"; exit 1; }
grep -q "VK_ERROR_FEATURE_NOT_PRESENT" "$W/nodri3.log" && { echo "FAIL: present returned VK_ERROR_FEATURE_NOT_PRESENT"; exit 1; }
# Measurement path: IMMEDIATE must be offered for a software swapchain (FIFO/MAILBOX stay for real apps).
grep -q "present modes: 0 1 2 3" "$W/nodri3.log" || { echo "FAIL: IMMEDIATE not advertised for the software path"; grep "present modes" "$W/nodri3.log"; exit 1; }
echo "present modes: 0(IMMEDIATE) 1 2 3; present branch: putimage, VK_SUCCESS"
echo "no-dri3: OK"
rc=$(cat "$W/xcb.rc")
if [ "$rc" = 77 ]; then exit 0; fi
[ "$rc" = 0 ] && grep -q '^PASS' "$W/xcb.log" || { echo "FAIL: t_vk_xcb rc=$rc"; exit 1; }
g=$(cat "$W/glx.rc")
if [ "$g" = skipped ]; then echo "glxinfo: skipped (not installed)"; exit 0; fi
ver=$(sed -n 's/^OpenGL version string: \([0-9][0-9.]*\).*/\1/p' "$W/glx.log" | head -1)
echo "glxinfo -B (zink): OpenGL version ${ver:-none}"
grep -E "OpenGL renderer|MESA: error|failed to" "$W/glx.log"
[ -n "$ver" ] || { echo "FAIL: glxinfo -B under X reports no GL version via zink (rc=$g)"; exit 1; }
grep -q "OpenGL renderer string: zink.*mali-own" "$W/glx.log" || { echo "FAIL: GL renderer is not zink on our device"; exit 1; }
grep -q "could not create swapchain" "$W/glx.log" && { echo "FAIL: zink reported a swapchain failure"; exit 1; }
echo "xwsi-host: OK"
