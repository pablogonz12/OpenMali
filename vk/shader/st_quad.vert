#version 450
// RUNG2 state sweep: full-screen quad from gl_VertexIndex (6 vertices) inside the case's 8x8 viewport.
// uv = q * pc.uvs.xy + pc.uvs.zw, clip z = pc.posz.x (so a case can put the quad beyond the far plane).
layout(location=0) out vec2 v_uv;
layout(push_constant) uniform P { vec4 uvs; vec4 col; vec4 posz; } pc;
const vec2 p[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
void main(){
  vec2 q = p[gl_VertexIndex];
  gl_Position = vec4(q * 2.0 - 1.0, pc.posz.x, 1.0);
  v_uv = q * pc.uvs.xy + pc.uvs.zw;
}
