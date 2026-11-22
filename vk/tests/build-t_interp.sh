#!/bin/bash
# SPDX-License-Identifier: MIT
# t_interp on its own: the interpolation qualifier sweep (smooth/noperspective/flat x plain/centroid/sample, with and
# without invariant, forward and reversed winding) compared against a CPU reference. vk/build.sh builds it too; this
# script is for rebuilding the test alone (host or cross) and running it.
#   tests/build-t_interp.sh [--build-only] [-- <t_interp args>]
#   MALI_TARGET=a64-glibc-wsi MESA_BUILD=../../third_party/build-vk-a64-glibc-wsi-restbl tests/build-t_interp.sh --build-only
set -euo pipefail
V=$(cd "$(dirname "$0")/.." && pwd); D=$(cd "$V/.." && pwd)
OUT=$V/out/${MALI_TARGET:-native}
build_only=0; args=()
for a in "$@"; do
   case $a in --build-only) build_only=1 ;; --) shift || true; args=("$@"); break ;; *) args+=("$a") ;; esac
done
# The compiler of the target: the Mesa build tree's own line for panvk_instance.c, the same source vk/build.sh uses
# (a cross target passes MESA_BUILD so this test is built for the phone, not for the host).
CC=${CC:-}
if [ -z "$CC" ] && [ -n "${MESA_BUILD:-}" ] && [ -f "$MESA_BUILD/build.ninja" ]; then
   CMDS=$(cd "$MESA_BUILD" && ninja -t commands src/panfrost/vulkan/libvulkan_panfrost.so)
   CCLINE=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS")
   CC=$(sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g; s#^([^ ]*/)?ccache +##' <<<"$CCLINE")
fi
CC=${CC:-cc}
mkdir -p "$OUT"
# shellcheck disable=SC2086
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter \
   -I"$D/third_party/mesa/include" -o "$OUT/t_interp" "$V/tests/t_interp.c" -ldl -lm
[ $build_only = 1 ] && exit 0
exec "$OUT/t_interp" "$@"
