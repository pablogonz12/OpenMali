#!/bin/bash
# SPDX-License-Identifier: MIT
# Usage: ./build.sh [native|aarch64|all]   (default: aarch64). Needs wayland-scanner on the host; the aarch64
# build links libwayland-client from the cross sysroot (droidtest/tools/sysroot). Output: out/t_present-<arch>
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out gen
CC_A64=${CC_A64:-$PWD/../../droidtest/tools/cc-aarch64-glibc}
KB=../libkbase
for p in xdg-shell linux-dmabuf-v1 presentation-time; do
  [ gen/$p-client-protocol.h -nt protocols/$p.xml ] || wayland-scanner client-header protocols/$p.xml gen/$p-client-protocol.h
  [ gen/$p-protocol.c -nt protocols/$p.xml ] || wayland-scanner private-code protocols/$p.xml gen/$p-protocol.c
done
CFLAGS="-std=gnu11 -O2 -g0 -Wall -Wextra -Wno-unused-parameter -I$KB/include -Igen"
SRC="t_present.c gen/xdg-shell-protocol.c gen/linux-dmabuf-v1-protocol.c gen/presentation-time-protocol.c $KB/src/ctx.c $KB/src/bo.c $KB/src/queue.c $KB/src/sync.c $KB/src/fb.c"
what=${1:-aarch64}
if [ "$what" = native ] || [ "$what" = all ]; then
  ${CC:-cc} $CFLAGS -o out/t_present-native $SRC -lwayland-client -lm
  echo "built out/t_present-native"
fi
if [ "$what" = aarch64 ] || [ "$what" = all ]; then
  "$CC_A64" $CFLAGS -o out/t_present-aarch64 $SRC -lwayland-client -lm
  llvm-strip -s out/t_present-aarch64 2>/dev/null || true
  echo "built out/t_present-aarch64"
fi
