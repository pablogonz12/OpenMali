#!/bin/sh
# provoking.vert.spv / provoking.frag.spv: VK_EXT_provoking_vertex pixel test (t_vk_feat provoking) - one covering triangle with a
# flat varying that differs per vertex (v0 red, v1 green, v2 blue), so FIRST must read v0's colour and LAST v2's.
cd "$(dirname "$0")" && glslangValidator -V --target-env vulkan1.3 provoking.vert -o provoking.vert.spv &&
   glslangValidator -V --target-env vulkan1.3 provoking.frag -o provoking.frag.spv
