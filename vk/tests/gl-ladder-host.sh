#!/bin/bash
# SPDX-License-Identifier: MIT
# The GL ladder (tools/gl-ladder.c, the wined3d present path: FBO -> glBlitFramebuffer -> swap) on the HOST through Mesa's
# zink on our ICD, with our own present dump (MALI_WSI_DUMP, vk/src/mali_wsi.c) as the instrument. Steps 0/1/3 of the ladder,
# no hardware: gamescope --backend headless gives an Xwayland with DRI3/Present/MIT-SHM, the fake backend serves the ICD, and
# MALI_WSI=sw pins the software (MIT-SHM) present path -- the only one that writes a dump.
#
# WHAT THIS CAN AND CANNOT PROVE. The dump is 0 in every step, and that is not a colour result: the fake backend executes
# nothing (mali_backend_fake.c: "nothing executes", the wait stands for the GPU) and the CPU rasteriser cannot run zink's
# first submit (mali_soft.c unsupported("compute frames") -> mali_cs_emit.c -ENOTSUP -> vkQueueSubmit VK_ERROR_DEVICE_LOST;
# zink clears, and our meta blits, are compute dispatches). So this test asserts the PATH and nothing about pixels:
#   * the renderer really is zink on our device (kopper -> xcb surface -> swapchain),
#   * the swapchain is created on the X11 platform with the SRGB swapchain format while the image is presented through a
#     UNORM view (the format half of the SRGB/UNORM split that the black-window hypothesis M1 names),
#   * the present takes our own MIT-SHM segment and the dump is written at the swapchain extent.
# Colour needs hardware: run the same ladder on a phone with MALI_WSI_DUMP and read the dump there.
# Usage: gl-ladder-host.sh <out dir with libvulkan_mali.so, mali_icd.json> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without gamescope/Xwayland, without gcc, or without tools/gl-ladder.c.
set -u
OUT=$1; GPU=${2:-v13}
T=$(cd "$(dirname "$0")" && pwd); V=$(cd "$T/.." && pwd); R=$(cd "$V/.." && pwd)
command -v gamescope >/dev/null && command -v Xwayland >/dev/null || { echo "skipped: no gamescope/Xwayland for a headless X server"; exit 0; }
command -v gcc >/dev/null || { echo "skipped: no gcc to build the ladder"; exit 0; }
[ -f "$R/tools/gl-ladder.c" ] || { echo "skipped: no tools/gl-ladder.c"; exit 0; }
[ -f "$OUT/mali_icd.json" ] || { echo "skipped: no mali_icd.json in $OUT"; exit 0; }
OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
OUT="$W/gl-ladder" timeout 120 "$R/tools/gl-ladder.sh" >"$W/build.log" 2>&1 || { echo "FAIL: cannot build the ladder"; tail -5 "$W/build.log"; exit 1; }
LAD=$(sed -n 's/^built: \([^ ]*\).*/\1/p' "$W/build.log" | head -1)
[ -x "$LAD" ] || { echo "FAIL: gl-ladder.sh built nothing"; cat "$W/build.log"; exit 1; }

cat >"$W/inner.sh" <<EOF
#!/bin/sh
# The gamescope WSI layer is off: it would turn these X11 swapchains into Wayland ones and no dump is written there.
export DISABLE_GAMESCOPE_WSI=1 ENABLE_GAMESCOPE_WSI=0 MALI_BACKEND=fake MALI_FAKE_GPU=$GPU
export VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json MESA_LOADER_DRIVER_OVERRIDE=zink
export MALI_WSI=sw MALI_WSI_DEBUG=1 MALI_WSI_DUMP=$W/d/f MALI_WSI_DUMP_EVERY=1
mkdir -p "$W/d"
for st in 0 1 3; do
   LADDER_STEP=\$st LADDER_HOLD=600 MALI_WSI_DUMP_EVERY=10 timeout 20 "$LAD" >"$W/st\$st.log" 2>&1
   echo \$? >"$W/st\$st.rc"
   for f in "$W/d"/f-*.ppm; do [ -f "\$f" ] && mv "\$f" "$W/d/s\${st}-\$(basename "\$f")"; done
done
EOF
chmod +x "$W/inner.sh"
timeout 150 gamescope --backend headless -W 800 -H 600 -- "$W/inner.sh" >"$W/gs.log" 2>&1
[ -f "$W/st0.rc" ] || { echo "FAIL: the ladder did not run inside gamescope"; grep -v alvr "$W/gs.log" | tail -10; exit 1; }

fail=0
for st in 0 1 3; do
   rc=$(cat "$W/st$st.rc" 2>/dev/null || echo 1)
   log=$W/st$st.log
   grep -vE 'alvr|Gamescope WSI|wsi acquire|wsi present #|wsi dump #' "$log" | grep -E 'renderer=|create_swapchain:|x11 present:|readback=|^step=' | sed "s/^/  step$st: /"
   [ "$rc" = 0 ] || { echo "FAIL: step $st exit $rc"; fail=1; }
   grep -q 'renderer="zink Vulkan.*mali-own' "$log" || { echo "FAIL: step $st: GL renderer is not zink on our device"; fail=1; }
   grep -qE 'wsi create_swapchain: platform=xcb format=44 .*-> VK_SUCCESS' "$log" \
      || { echo "FAIL: step $st: no X11 swapchain in the SRGB format (B8G8R8A8_SRGB=44)"; fail=1; }
   grep -qE 'x11 present: shm=1 putimage=0' "$log" || { echo "FAIL: step $st: MIT-SHM present path not taken (no dump is written on any other)"; fail=1; }
   grep -qE 'wsi present image [0-9]+ path shm result VK_SUCCESS: VK_FORMAT_B8G8R8A8_UNORM 800x600' "$log" \
      || { echo "FAIL: step $st: not presented through the UNORM view of the 800x600 swapchain image"; fail=1; }
   n=0
   for f in "$W/d"/s$st-*.ppm; do [ -f "$f" ] && n=$((n+1)); done
   [ "$n" -ge 1 ] || { echo "FAIL: step $st: our dump wrote no frame"; fail=1; }
   for f in "$W/d"/s$st-*.ppm; do
      [ -f "$f" ] || continue
      echo "  step$st: dump $(basename "$f") $(magick "$f" -format '%wx%h mean=%[fx:int(mean*25500)/100]' info: 2>/dev/null || sed -n '2p' "$f" | head -1)"
   done
done
[ "$fail" = 0 ] || { echo "FAIL: gl ladder host"; exit 1; }
echo "swapchain: platform=xcb format=44 (B8G8R8A8_SRGB), presented as VK_FORMAT_B8G8R8A8_UNORM -- the SRGB/UNORM split M1 names"
echo "present: shm=1 (our segment, dump written); ladder steps 0/1/3 ran through zink on our ICD"
echo "pixels: 0 by construction on the fake backend (nothing executes; mali_soft.c rejects zink's compute frames)"
echo "gl-ladder-host: OK (path only, no colour -- colour needs hardware: MalI_WSI_DUMP on a phone)"
