#!/bin/sh
# arraylen.comp.spv (SSBO .length(): get_ssbo_size is refused), pushwide.frag.spv (128 byte push block read by a fragment shader)
cd "$(dirname "$0")" && for s in arraylen.comp pushwide.frag; do glslangValidator -V --target-env vulkan1.3 $s -o $s.spv || exit 1; done
