#!/bin/sh
# bda_frag.frag.spv: buffer_reference store in a FRAGMENT shader (global derefs were only lowered for compute)
cd "$(dirname "$0")" && glslangValidator -V --target-env vulkan1.3 bda_frag.frag -o bda_frag.frag.spv
