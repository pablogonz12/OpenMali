#!/bin/bash
# SPDX-License-Identifier: MIT
# Host smoke test of Mesa's zink on our ICD (fake backend, no hardware): EGL device platform (the one without a DRM render fd, so
# zink's EXT_physical_device_drm check does not apply), GLES2 context, one triangle, glReadPixels. Pixels are only real on hardware; here
# the gate is "context created on our device, shaders compile, draw and readback return GL_NO_ERROR".
# Skips (exit 0, prints "skipped") when EGL/GLES headers or Mesa's zink are not on the host. Usage: zink-host.sh <icd.so dir> <gpu v10|v11|v13>
set -u
OUT=$1; GPU=${2:-v10}
T=$(dirname "$0"); W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
[ -f /usr/include/EGL/egl.h ] && [ -f /usr/include/GLES2/gl2.h ] || { echo "skipped: no EGL/GLES2 headers"; exit 0; }
ls /usr/lib/libgallium-*.so >/dev/null 2>&1 || { echo "skipped: no Mesa gallium"; exit 0; }
gcc "$T/zink_tri.c" -o "$W/tri" -lEGL -lGLESv2 || { echo "FAIL: cannot build zink_tri"; exit 1; }
# device index: the last EGL device is the software/no-fd one (index 1 on hosts with one GPU)
log=$(MESA_LOADER_DRIVER_OVERRIDE=zink MALI_BACKEND=fake MALI_FAKE_GPU=$GPU VK_DRIVER_FILES="$OUT/mali_icd.json" timeout 120 stdbuf -o0 "$W/tri" 1 2>&1 | grep -v alvr)
echo "$log"
echo "$log" | grep -q "GL_RENDERER zink Vulkan.*mali-own" || { echo "FAIL: zink did not create a context on our device"; exit 1; }
echo "$log" | grep -q "glGetError 0x0" || { echo "FAIL: GL error or crash after context creation"; exit 1; }
