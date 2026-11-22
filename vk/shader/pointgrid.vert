#version 450
// a 16x16 grid of POINT_LIST vertices; gl_PointSize comes from a push constant (pc.x), 0.0 = leave 1.0
layout(push_constant) uniform P { vec4 s; } pc;
void main(){
  int i = gl_VertexIndex;
  float x = float(i & 15) / 16.0 * 2.0 - 1.0 + 1.0 / 16.0;
  float y = float(i >> 4) / 16.0 * 2.0 - 1.0 + 1.0 / 16.0;
  gl_Position = vec4(x, y, 0.5, 1.0);
  gl_PointSize = pc.s.x > 0.0 ? pc.s.x : 1.0;
}
