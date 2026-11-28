#version 450
// Varying budget: 16 user vec4 outputs (= maxVertexOutputComponents 64) + gl_CullDistance[8] (= maxCullDistances,
// = maxCombinedClipAndCullDistances): the two cull flag slots come on top and the VS and FS must agree on them.
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[8]; };
layout(location = 0) out vec4 v[16];
const vec2 p[6] = vec2[6](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
void main(){
  vec2 n = p[gl_VertexIndex] * 2.0 - 1.0;
  gl_Position = vec4(n, 0.5, 1.0);
  for (int i = 0; i < 16; i++) v[i] = vec4(float(i) / 16.0, 0.0, 0.0, 1.0);
  for (int i = 0; i < 8; i++) gl_CullDistance[i] = 1.0;
  gl_CullDistance[7] = n.x;                    /* the last flag of the second slot decides */
}
