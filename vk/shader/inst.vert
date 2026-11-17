#version 450
// R4-B2 t_vk_feat: right triangle (legs 0.5 NDC) shifted in x by 0.5 * gl_InstanceIndex - 0.75: first/instance and large index tests.
const vec2 p[3] = vec2[3](vec2(-.25,-.25), vec2(.25,-.25), vec2(-.25,.25));
void main(){
  vec2 q = p[gl_VertexIndex] + vec2(0.5 * float(gl_InstanceIndex) - 0.75, 0.0);
  gl_Position = vec4(q, 0.5, 1.0);
}
