#!/bin/bash
# SPDX-License-Identifier: MIT
# GLSL -> SPIR-V for the t_compile tests. The .spv files are committed so the tests need no glslang.
# Vulkan 1.1 for the graphics shaders (as R2); the compute stub uses buffer device address, so Vulkan 1.2.
set -e; cd "$(dirname "$0")"
for s in *.vert *.frag *.geom; do glslangValidator -V --target-env vulkan1.1 "$s" -o "$s.spv" >/dev/null; done
for s in *.comp; do glslangValidator -V --target-env vulkan1.2 "$s" -o "$s.spv" >/dev/null; done
