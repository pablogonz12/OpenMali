#version 450
// t_vk_feat divisor: right triangle (legs 0.5 NDC) at column 0 (x -0.75), shifted right by the instance-rate attribute (0, 0.5, 1, 1.5 = columns 0..3).
layout(location = 0) in float ofs;
const vec2 p[3] = vec2[3](vec2(-.25,-.25), vec2(.25,-.25), vec2(-.25,.25));
void main(){
  vec2 q = p[gl_VertexIndex] + vec2(ofs - 0.75, 0.0);
  gl_Position = vec4(q, 0.5, 1.0);
}
