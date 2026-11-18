#!/bin/bash
# SPDX-License-Identifier: MIT
# One compute shader per storage-image format (t_vk_feat --only storfmt). Kind: f = vec4(1,0,1,1), u = uvec4(1,2,3,1), i = ivec4(-1,2,-3,1).
set -e; cd "$(dirname "$0")"
while read -r q k; do
  case $k in f) ty=image2D; v='vec4(1.0, 0.0, 1.0, 1.0)';; u) ty=uimage2D; v='uvec4(1u, 2u, 3u, 1u)';; i) ty=iimage2D; v='ivec4(-1, 2, -3, 1)';; esac
  cat > stor_$q.comp <<EOF
#version 450
layout(local_size_x = 4, local_size_y = 4) in;
layout($q, set = 0, binding = 0) uniform writeonly $ty img;
void main() { imageStore(img, ivec2(gl_GlobalInvocationID.xy), $v); }
EOF
  glslangValidator -V --target-env vulkan1.2 stor_$q.comp -o stor_$q.comp.spv >/dev/null
done <<'EOF'
r8 f
r8_snorm f
r8ui u
r8i i
rg8 f
rg8_snorm f
rg8ui u
rg8i i
rgb10_a2 f
rgb10_a2ui u
r16 f
r16_snorm f
r16ui u
r16i i
r16f f
rg16 f
rg16_snorm f
rg16ui u
rg16i i
rg16f f
rgba16 f
rgba16_snorm f
rgba16ui u
rgba16i i
rgba16f f
rg32ui u
rg32i i
rg32f f
r11f_g11f_b10f f
EOF
# shaderStorageImageWriteWithoutFormat (gamescope): the same stores with no format qualifier; the texel format comes from the image view (t_vk_feat --only storfmt_nf)
for k in f u i; do
  case $k in f) ty=image2D; v='vec4(1.0, 0.0, 1.0, 1.0)';; u) ty=uimage2D; v='uvec4(1u, 2u, 3u, 1u)';; i) ty=iimage2D; v='ivec4(-1, 2, -3, 1)';; esac
  cat > stornf_$k.comp <<EOF
#version 450
layout(local_size_x = 4, local_size_y = 4) in;
layout(set = 0, binding = 0) uniform writeonly $ty img;
void main() { imageStore(img, ivec2(gl_GlobalInvocationID.xy), $v); }
EOF
  glslangValidator -V --target-env vulkan1.2 stornf_$k.comp -o stornf_$k.comp.spv >/dev/null
done
