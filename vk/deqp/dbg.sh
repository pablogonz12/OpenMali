#!/bin/sh
# SPDX-License-Identifier: MIT
# Guest: run the case list at Download/qdeqp/dbg.cases with our ICD and MALI_PIPE_DBG=1; prints unsupported() reasons (histogram). Needs a prior qdeqp --keep run (/tmp/qd).
D=/storage/emulated/0/Download/qdeqp; cd /tmp/qd || exit 1
cp $D/libvulkan_mali.so /tmp/qd/libvulkan_mali.so
printf '{"file_format_version":"1.0.1","ICD":{"library_path":"/tmp/qd/libvulkan_mali.so","api_version":"1.3.0"}}' >/tmp/qd/icd.json
VK_DRIVER_FILES=/tmp/qd/icd.json VK_ICD_FILENAMES=/tmp/qd/icd.json VK_LOADER_LAYERS_DISABLE='~all~' BL_OPEN_DRIVER=1 MALI_PIPE_DBG=1 \
 timeout 100 ./deqp-vk --deqp-caselist-file=$D/dbg.cases --deqp-log-filename=/tmp/qd/dbg.qpa --deqp-log-images=disable --deqp-surface-type=fbo --deqp-gl-config-name=rgba8888d24s8 --deqp-visibility=hidden --deqp-shadercache=disable >/tmp/qd/dbg.out 2>&1
grep -a "mali_\|unsupported\|Warn\|Fail" /tmp/qd/dbg.out | sort | uniq -c | sort -rn | head -12 | sed 's/\\/\\\\/g; s/"/\\"/g; s/^\(.*\)$/{"line":"\1"}/'
echo '{"test":"summary","ok":true}'
