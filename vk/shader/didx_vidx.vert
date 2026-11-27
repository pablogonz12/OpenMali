#version 450
// descriptor indexing, index carried by a flat int varying from a vertex shader (the CTS descriptorIndexing_vert* pattern)
layout(location=0) out vec2 v_uv;
layout(location=1) flat out int k;
layout(push_constant) uniform P { vec4 s; vec4 i; } pc;
const vec2 p[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
void main(){
  vec2 q = p[gl_VertexIndex];
  gl_Position = vec4(q * 2.0 - 1.0, 0.5, 1.0);
  v_uv = q * pc.s.xy + pc.s.zw;
  k = int(pc.i.x);
}
