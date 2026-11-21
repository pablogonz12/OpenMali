#!/bin/bash
# SPDX-License-Identifier: MIT
# GLSL -> SPIR-V for the blitreplay test (notes/blitreplay.md): the replay's own shaders plus the binding/set variants
# the bisect needs. The dumped zink fragment shader is committed byte-identical as blitfs_z.frag.spv (its source is
# tools/dump/spv-frag-3.spvasm on lane/blit-instr); this script does not touch it.
set -e; cd "$(dirname "$0")"
V="glslangValidator -V --target-env vulkan1.1"
$V blitv_z.vert -o blitv_z.vert.spv >/dev/null
$V blitv_o.vert -o blitv_o.vert.spv >/dev/null
$V blitfs_o.frag -o blitfs_o.frag.spv >/dev/null                       # set 2, binding 128 (the frame's layout)
$V -DFS_BIND=0 blitfs_o.frag -o blitfs_o_b0.frag.spv >/dev/null        # bisect: binding 128 -> 0
$V -DFS_SET=0  blitfs_o.frag -o blitfs_o_s0.frag.spv >/dev/null        # bisect: set 2 -> set 0
ls -l blitv_*.spv blitfs_*.spv
sha256sum blitfs_z.frag.spv
