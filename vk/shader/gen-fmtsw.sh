#!/bin/bash
# SPDX-License-Identifier: MIT
# GLSL -> SPIR-V for the format sweep (notes/format-sweep.md). Three vertex modules (float / uint / sint attribute
# classes) and three fragment modules (the same classes for the sampled-format rows).
set -e; cd "$(dirname "$0")"
V="glslangValidator -V --target-env vulkan1.1"
$V fmtsw.vert -o fmtsw.vert.spv >/dev/null                                        # vec4/uvec4/ivec4 class: float
$V -DVT=uvec4 -DV2T=uvec2 fmtsw.vert -o fmtsw_u.vert.spv >/dev/null               # unsigned integer attributes
$V -DVT=ivec4 -DV2T=ivec2 fmtsw.vert -o fmtsw_s.vert.spv >/dev/null               # signed integer attributes
$V fmtsw.frag -o fmtsw.frag.spv >/dev/null
$V fmtsw_quad.vert -o fmtsw_quad.vert.spv >/dev/null
$V fmtsw_tex.frag -o fmtsw_tex.frag.spv >/dev/null
$V -DIS_UINT fmtsw_tex.frag -o fmtsw_tex_u.frag.spv >/dev/null
$V -DIS_SINT fmtsw_tex.frag -o fmtsw_tex_s.frag.spv >/dev/null
ls -l fmtsw*.spv
