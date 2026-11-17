#!/bin/bash
# SPDX-License-Identifier: MIT
# dEQP-VK (vk-gl-cts vulkan-cts-1.3.10.0) for aarch64 glibc (guest). Source: driver/third_party/cts-src (fetched, pinned tag).
# ./build.sh  ->  driver/vk/deqp/out/a64-glibc/{deqp-vk,external/vulkancts/data/...}. Resumable; ~1-2 h on 4 cores.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); S=$H/../../third_party/cts-src; O=$H/out/a64-glibc
export PATH=$H/../../third_party/venv/bin:$PATH
[ -f $O/build.ninja ] || cmake -S $S -B $O -G Ninja -DCMAKE_TOOLCHAIN_FILE=$H/a64-glibc.cmake -DCMAKE_BUILD_TYPE=Release \
  -DDEQP_TARGET=default -DCMAKE_DISABLE_FIND_PACKAGE_Wayland=ON -DCMAKE_DISABLE_FIND_PACKAGE_X11=ON -DDEQP_USE_X11=OFF -DDEQP_USE_WAYLAND=OFF -DDEQP_USE_XCB=OFF \
  -DDEQP_USE_HEADLESS=ON -DDEQP_SUPPORT_GLES2=OFF -DDEQP_SUPPORT_GLES3=OFF -DDEQP_SUPPORT_EGL=OFF -DDEQP_SUPPORT_OPENGL=OFF \
  -DDEQP_ANDROID_EXE=OFF -DSELECTED_BUILD_TARGETS=deqp-vk -DPYTHON_EXECUTABLE=/usr/bin/python3
ninja -C $O ${JOBS:+-j$JOBS} deqp-vk
