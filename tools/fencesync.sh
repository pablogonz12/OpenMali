#!/bin/bash
# SPDX-License-Identifier: MIT
# Build tools/fencesync.c: the glFenceSync/EGL-fence loop that asks whether the first error of the
# GPU-composited Steam menu on x8 (ANGLE-GL SyncGL.cpp:42, 0x00000505 GL_OUT_OF_MEMORY) is a zink
# fence-path need our Vulkan driver refuses. GL and EGL entry points are resolved at run time, so
# nothing GL/EGL is needed at link time -- the aarch64 sysroot ships X11 but no GL headers.
#
# On the phone the binary runs inside a DroidDeck session (gamescope's X server, DISPLAY=:0):
#   ~/.config/opencode/mali-orchestrator/bin/ddc x4 run sh -c \
#     'FENCE_VARIANT=gl FENCE_ITERS=1000 VK_DRIVER_FILES=/root/fs/mali_icd.json \
#      MESA_LOADER_DRIVER_OVERRIDE=zink /root/fencesync'
set -eu
R=$(cd "$(dirname "$0")/.." && pwd)
SRC=$R/tools/fencesync.c
M=${MALI_ORCH:-$HOME/.config/opencode/mali-orchestrator}
mode=host
push=0
for a in "$@"; do
    case "$a" in
        --cross) mode=cross ;;
        --push) push=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "usage: tools/fencesync.sh [--cross [--push]]" >&2; exit 2 ;;
    esac
done

if [ "$mode" = host ]; then
    out=${OUT:-/tmp/fencesync}
    gcc -O1 -Wall -Wextra -o "$out" "$SRC" -lX11 -ldl
    echo "built: $out (host $(uname -m))"
    echo "run:   FENCE_VARIANT=gl|egl FENCE_ITERS=1000 $out"
    exit 0
fi

# ---- cross build for the guest ---------------------------------------------------------------
SR=${FENCESYNC_SYSROOT:-$HOME/Projects/mali-gaming/droidtest/tools/sysroot}
[ -f "$SR/usr/include/X11/Xlib.h" ] || { echo "no X11 headers in $SR (set FENCESYNC_SYSROOT)" >&2; exit 1; }
gccdir=$(ls -d "$SR"/usr/lib/gcc/aarch64-linux-gnu/* 2>/dev/null | sort -V | tail -1)
[ -n "$gccdir" ] || { echo "no aarch64 gcc runtime in $SR" >&2; exit 1; }
out=${OUT:-$R/fencesync-a64}
clang --target=aarch64-linux-gnu --sysroot="$SR" --gcc-install-dir="$gccdir" \
      -fuse-ld=lld -rtlib=libgcc -Qunused-arguments -O1 -Wall -Wextra \
      -o "$out" "$SRC" -lX11 -ldl
case "$(file -b "$out")" in
    *aarch64*) ;;
    *) echo "FAIL: $out is not aarch64"; exit 1 ;;
esac
echo "built: $out ($(stat -c%s "$out") bytes, $(file -b "$out" | cut -d, -f1-2))"
echo "push:  FENCESYNC_PHONE=x4 tools/fencesync.sh --cross"
