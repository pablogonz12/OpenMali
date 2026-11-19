#!/bin/sh
# Mesa-side asserts reached by fuzzed shaders; mali_compile.c legalizes them before the Mesa passes (t_compile test_legalize)
cd "$(dirname "$0")" && for s in legalize_i8 legalize_cmp; do glslangValidator -V --target-env vulkan1.3 $s.comp -o $s.comp.spv; done
