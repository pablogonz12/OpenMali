#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest-side X11 WSI check on hardware (STEAM-GAPS.md "X11 WSI"), via scripts/quick/qdrv. Needs Download/x11wsi_{libvulkan_mali.so,t_vk_xcb}
# (out/a64-glibc-wsi: ./cross/build-cross.sh glibc-wsi) and a running guest X server ($DISPLAY, e.g. gamescope's Xwayland).
# Runs t_vk_xcb three ways and records what the X server offers (the answer to "does the guest's Xwayland have DRI3/Present"):
#   sw       MALI_WSI=sw      CPU-visible linear buffer + MIT-SHM/PutImage. The path that must work with a DRM-less Xwayland. --pixels.
#   dmabuf   MALI_WSI=dmabuf  DRI3 pixmap_from_buffer of our dma-buf. Expected to fail where Xwayland has no DRI3; its failure is data, not a fail.
#   default  (no MALI_WSI)    the hardware default (= dmabuf).
# Pass = the sw run passes with correct pixels. The gamescope WSI layer is disabled (it would redirect the X11 swapchain to Wayland).
D=/storage/emulated/0/Download; T=/tmp/x11wsi; rm -rf $T; mkdir -p $T
cp $D/x11wsi_libvulkan_mali.so $T/libvulkan_mali.so; cp $D/x11wsi_t_vk_xcb $T/t_vk_xcb; chmod +x $T/t_vk_xcb
cat >$T/icd.json <<J
{"file_format_version":"1.0.1","ICD":{"library_path":"$T/libvulkan_mali.so","api_version":"1.3.0"}}
J
LOG=$T/log; :>$LOG; ok=1
export DISPLAY=${DISPLAY:-:0} ENABLE_GAMESCOPE_WSI=0 VK_DRIVER_FILES=$T/icd.json VK_ICD_FILENAMES=$T/icd.json
if command -v xdpyinfo >/dev/null 2>&1; then
   echo "== xdpyinfo $DISPLAY" >>$LOG
   xdpyinfo -queryExtensions 2>&1 | grep -iE "DRI3|Present|MIT-SHM|GLX|XFIXES|SYNC|RANDR|glamor|can't open" >>$LOG
else echo "# no xdpyinfo in guest" >>$LOG; fi
echo "== t_vk_xcb sw --pixels" >>$LOG
MALI_WSI=sw timeout 60 $T/t_vk_xcb --direct $T/libvulkan_mali.so --pixels >>$LOG 2>&1; rc=$?
echo "rc=$rc" >>$LOG; [ $rc = 0 ] || ok=0
echo "== t_vk_xcb dmabuf (informational)" >>$LOG
MALI_WSI=dmabuf timeout 60 $T/t_vk_xcb --direct $T/libvulkan_mali.so >>$LOG 2>&1; echo "rc=$?" >>$LOG
echo "== t_vk_xcb default (informational)" >>$LOG
timeout 60 $T/t_vk_xcb --direct $T/libvulkan_mali.so >>$LOG 2>&1; echo "rc=$?" >>$LOG
tr '\t' ' ' <$LOG >$LOG.2; mv $LOG.2 $LOG
sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/' $LOG
echo "{\"test\":\"summary\",\"ok\":$([ $ok = 1 ] && echo true || echo false)}"
