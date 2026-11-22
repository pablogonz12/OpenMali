#version 450
// RUNG2 state sweep: explicit-LOD sample, the LOD comes from the push constant (pc.col.x).
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_color;
layout(set=0, binding=0) uniform sampler2D tex;
layout(push_constant) uniform P { vec4 uvs; vec4 col; vec4 posz; } pc;
void main(){ out_color = textureLod(tex, v_uv, pc.col.x); }
