#!/bin/bash
# Build the GL blit ladder and print how to run it.
#
#   tools/gl-ladder.sh                 build for this host (x86_64 + Mesa) and print the run command
#   tools/gl-ladder.sh --cross         cross-build for the phone guest (aarch64 Ubuntu sysroot, clang + lld)
#   tools/gl-ladder.sh --cross --push  … and push it to a phone first: GL_LADDER_PHONE=x4
#
# What the phone gets: gl-ladder.c loads libGL.so.1 with dlopen and resolves every GL/GLX entry
# point at run time, so nothing GL is needed at link time -- which matters because neither aarch64
# sysroot on this host ships GL headers or libGL. X11 headers and libX11 do come from the sysroot.
#
# On the phone the binary is run inside a DroidDeck session (gamescope's X server, DISPLAY=:0):
#   ~/.config/opencode/mali-orchestrator/bin/ddc "$GL_LADDER_PHONE" override gl-ladder <binary>
#   ~/.config/opencode/mali-orchestrator/bin/ddc "$GL_LADDER_PHONE" run sh -c 'LADDER_STEP=1 LADDER_HOLD=20000 ./gl-ladder'
set -eu
R=$(cd "$(dirname "$0")/.." && pwd)
SRC=$R/tools/gl-ladder.c
M=${MALI_ORCH:-$HOME/.config/opencode/mali-orchestrator}
mode=host
push=0
for a in "$@"; do
    case "$a" in
        --cross) mode=cross ;;
        --push) push=1 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "usage: tools/gl-ladder.sh [--cross [--push]]" >&2; exit 2 ;;
    esac
done

if [ "$mode" = host ]; then
    out=${OUT:-/tmp/gl-ladder}
    gcc -O1 -Wall -Wextra -o "$out" "$SRC" -lX11 -ldl
    echo "built: $out (host $(uname -m))"
    echo "run:   for n in 1 2 3 4; do DISPLAY=\$DISPLAY LADDER_STEP=\$n LADDER_HOLD=800 $out; done"
    exit 0
fi

# ---- cross build for the guest ---------------------------------------------------------------
SR=${GL_LADDER_SYSROOT:-$HOME/Projects/mali-gaming/panvk-build/sysroot}
[ -f "$SR/usr/include/X11/Xlib.h" ] || { echo "no X11 headers in $SR (set GL_LADDER_SYSROOT)" >&2; exit 1; }
gccdir=$(ls -d "$SR"/usr/lib/gcc/aarch64-linux-gnu/* 2>/dev/null | sort -V | tail -1)
[ -n "$gccdir" ] || { echo "no aarch64 gcc runtime in $SR" >&2; exit 1; }
out=${OUT:-$R/gl-ladder-a64}
clang --target=aarch64-linux-gnu --sysroot="$SR" --gcc-install-dir="$gccdir" \
      -fuse-ld=lld -rtlib=libgcc -Qunused-arguments -O1 -Wall -Wextra \
      -o "$out" "$SRC" -lX11 -ldl
case "$(file -b "$out")" in
    *aarch64*) ;;
    *) echo "FAIL: $out is not aarch64"; exit 1 ;;
esac
echo "built: $out ($(stat -c%s "$out") bytes, $(file -b "$out" | cut -d, -f1-2))"

phone=${GL_LADDER_PHONE:-}
if [ "$push" = 1 ]; then
    [ -n "$phone" ] || { echo "set GL_LADDER_PHONE=x8|cmf|x4 to --push" >&2; exit 2; }
    # override puts the binary where the session finds it; it is per-session state, not a driver change
    "$M/bin/ddc" "$phone" override gl-ladder "$out" >/dev/null
    echo "pushed: $phone gl-ladder"
    echo "run:   $M/bin/ddc $phone run sh -c 'LADDER_STEP=1 LADDER_HOLD=20000 ./gl-ladder'"
else
    echo "push:  GL_LADDER_PHONE=x4 tools/gl-ladder.sh --cross --push"
fi
echo "steps: LADDER_STEP=1 (FBO clear+blit) … 4 (sRGB FBO + second window); LADDER_DUMP=/root/x.ppm saves the readback"
