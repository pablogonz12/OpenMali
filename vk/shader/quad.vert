#version 450
// R4-B2 t_vk_feat: full-screen quad from gl_VertexIndex (6 vertices). uv = pos01 * pc.xy + pc.zw.
layout(location=0) out vec2 v_uv;
layout(push_constant) uniform P { vec4 s; } pc;
const vec2 p[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
void main(){
  vec2 q = p[gl_VertexIndex];
  gl_Position = vec4(q * 2.0 - 1.0, 0.5, 1.0);
  v_uv = q * pc.s.xy + pc.s.zw;
}
