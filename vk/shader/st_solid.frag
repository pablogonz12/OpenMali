#version 450
// RUNG2 state sweep: constant colour from the push constant (blend source, depth/stencil colour result).
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_color;
layout(push_constant) uniform P { vec4 uvs; vec4 col; vec4 posz; } pc;
void main(){ out_color = pc.col; }
