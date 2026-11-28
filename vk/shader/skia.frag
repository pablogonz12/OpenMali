#version 450
// t_skia_draws: texture (binding 0) modulated by a dynamic uniform buffer colour (binding 1).
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform sampler2D tex;
layout(set=0, binding=1) uniform U { vec4 colour; } u;
void main(){ o = texture(tex, v_uv) * u.colour; }
