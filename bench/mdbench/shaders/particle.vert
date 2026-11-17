#version 450
layout(location=0) in vec4 inst;     // per instance: xy pos (-1..1), z phase, w size
layout(location=0) out vec3 col;
void main(){
  vec2 q[6] = vec2[](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
  vec2 c = q[gl_VertexIndex % 6];
  float s = 0.004 + 0.004 * inst.w;
  gl_Position = vec4(inst.xy + c * s, 0.5, 1.0);
  col = 0.5 + 0.5*cos(vec3(0.0,2.1,4.2) + inst.z*6.28);
}
