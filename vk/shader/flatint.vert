#version 450
// flat int varying VS -> FS, value from a push constant (no descriptors involved)
layout(location=0) flat out int k;
layout(push_constant) uniform P { vec4 s; vec4 i; } pc;
const vec2 p[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
void main(){ vec2 q = p[gl_VertexIndex]; gl_Position = vec4(q * 2.0 - 1.0, 0.5, 1.0); k = int(pc.i.x); }
