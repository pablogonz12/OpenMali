#!/bin/bash
# SPDX-License-Identifier: MIT
# Usage: tests/build-t_state_sweep.sh [native|aarch64|all]   Output: out/t_state_sweep-<arch>
# Standalone Vulkan test for the RUNG2 state sweep (sampler / blend / depth-stencil); it loads the ICD itself, so it
# links nothing but libdl/libm. Shaders: vk/shader/st_{quad.vert,tex,impl,solid}.frag.spv (run shader/gen-spv.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
CC_A64=${CC_A64:-$PWD/../../droidtest/tools/cc-aarch64-glibc}
CF="-std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -I../third_party/mesa/include"
SRC="tests/t_state_sweep.c"
mkdir -p out
what=${1:-aarch64}
case "$what" in
   native|aarch64|all) ;;
   *) echo "unknown target '$what': use native|aarch64|all"; exit 2;;
esac
if [ "$what" = native ] || [ "$what" = all ]; then
   ${CC:-cc} $CF -o out/t_state_sweep-native $SRC -ldl -lm
   echo "built out/t_state_sweep-native"
fi
if [ "$what" = aarch64 ] || [ "$what" = all ]; then
   [ -x "$CC_A64" ] || { echo "missing cross compiler $CC_A64"; exit 1; }
   "$CC_A64" $CF -o out/t_state_sweep-aarch64 $SRC -ldl -lm
   llvm-strip -s out/t_state_sweep-aarch64 2>/dev/null || true
   echo "built out/t_state_sweep-aarch64"
fi
