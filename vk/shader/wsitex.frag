#version 450
// t_vk_xcb --textured: the 2x2 texture's colour, sampled with the quad uv (quad.vert: v_uv = q).
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main(){ o = vec4(texture(u_tex, v_uv).rgb, 1.0); }
