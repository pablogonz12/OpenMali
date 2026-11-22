#version 450
/* textureLod at a pushed explicit LOD, NEAREST sampler: t_vk_mip checks that the descriptor reads the level it is told. */
layout(push_constant) uniform PC { float lod; float scale; } pc;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 o;
void main() { o = textureLod(tex, vec2(0.5, 0.5), pc.lod); }
