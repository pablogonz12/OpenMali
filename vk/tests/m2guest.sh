#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest-side M2 check, run through scripts/quick/qdrv (the "binary"). Files are pushed to Download/m2vk_* first.
D=/storage/emulated/0/Download; T=/tmp/m2vk; rm -rf $T; mkdir -p $T
cp $D/m2vk_libvulkan_mali.so $T/libvulkan_mali.so; cp $D/m2vk_t_icd $T/t_icd; chmod +x $T/t_icd; [ -e $D/m2vk_vulkaninfo ] && cp $D/m2vk_vulkaninfo $T/vulkaninfo && cp $D/m2vk_libvulkan.so.1 $T/libvulkan.so.1 && chmod +x $T/vulkaninfo
echo "{\"library_path\":\"$T/libvulkan_mali.so\"}" >/dev/null
cat >$T/icd.json <<J
{"file_format_version":"1.0.1","ICD":{"library_path":"$T/libvulkan_mali.so","api_version":"1.3.0"}}
J
ok=1
LOG=$T/log
:>$LOG
WANT=${M2_WANT:-Mali}
run_all() {
echo "== direct"; $T/t_icd --direct $T/libvulkan_mali.so --expect-name "$WANT"; [ $? = 0 ] || ok=0
if [ -e /usr/lib/aarch64-linux-gnu/libvulkan.so.1 ] || ldconfig -p 2>/dev/null | grep -q libvulkan.so.1; then
  export VK_DRIVER_FILES=$T/icd.json VK_ICD_FILENAMES=$T/icd.json VK_LOADER_LAYERS_DISABLE='~all~'
  echo "== loader"; $T/t_icd --expect-name "$WANT"; [ $? = 0 ] || ok=0
  if [ -x $T/vulkaninfo ]; then echo "== vulkaninfo"; LD_LIBRARY_PATH=$T $T/vulkaninfo --summary 2>&1 | sed 's/^/# /'; else echo "# no vulkaninfo in guest"; fi
else echo "# no libvulkan.so.1 in guest"; fi
echo "# fds:"; ls -l /proc/self/fd 2>/dev/null | grep -c mali
echo $ok >$T/ok
}
run_all >$LOG 2>&1
ok=$(cat $T/ok)
sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/' $LOG
echo "{\"test\":\"summary\",\"ok\":$([ $ok = 1 ] && echo true || echo false)}"
