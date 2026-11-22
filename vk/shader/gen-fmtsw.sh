#!/bin/bash
# SPDX-License-Identifier: MIT
# GLSL -> SPIR-V for the format sweep (notes/format-sweep.md): the attribute vertex stage in three numeric classes and
# two widths, plus the fragment stages (attributes, and the sampled-format fetches).
set -e; cd "$(dirname "$0")"
V="glslangValidator -V --target-env vulkan1.1"
for w in 4 2; do
   [ $w = 2 ] && D="-DVEC2"
   $V $D                 fmtsw.vert -o fmtsw$w.vert.spv >/dev/null
   $V $D -DVT=uvec4 -DVT2=uvec2 fmtsw.vert -o fmtsw${w}_u.vert.spv >/dev/null
   $V $D -DVT=ivec4 -DVT2=ivec2 fmtsw.vert -o fmtsw${w}_s.vert.spv >/dev/null
done
$V fmtsw.frag -o fmtsw.frag.spv >/dev/null
$V fmtsw_quad.vert -o fmtsw_quad.vert.spv >/dev/null
$V fmtsw_tex.frag -o fmtsw_tex.frag.spv >/dev/null
$V -DIS_UINT fmtsw_tex.frag -o fmtsw_tex_u.frag.spv >/dev/null
$V -DIS_SINT fmtsw_tex.frag -o fmtsw_tex_s.frag.spv >/dev/null
ls -l fmtsw*.spv
