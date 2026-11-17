#!/bin/bash
# SPDX-License-Identifier: MIT
# Usage: ./build.sh [native|aarch64|all]   (default: all). Outputs: out/probe-native, out/probe-aarch64
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
CFLAGS="-std=c11 -O2 -g0 -Wall -Wextra -Werror -pedantic"
CC_A64=${CC_A64:-$PWD/../../droidtest/tools/cc-aarch64-glibc}
what=${1:-all}
if [ "$what" = native ] || [ "$what" = all ]; then
  ${CC:-cc} $CFLAGS -o out/probe-native probe.c
  echo "built out/probe-native"
fi
if [ "$what" = aarch64 ] || [ "$what" = all ]; then
  # dynamic link against the Ubuntu arm64 glibc sysroot (same libc family as the guest); no -static.
  "$CC_A64" $CFLAGS -Wl,--as-needed -o out/probe-aarch64 probe.c
  llvm-strip -s out/probe-aarch64 2>/dev/null || true
  echo "built out/probe-aarch64"
fi
