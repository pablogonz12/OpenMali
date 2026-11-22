#version 450
// a dim x dim grid of POINT_LIST vertices; gl_PointSize from pc.x, the grid dimension from pc.y
layout(push_constant) uniform P { vec4 s; } pc;
void main(){
  int i = gl_VertexIndex;
  int dim = int(pc.s.y + 0.5);
  float x = float(i % dim) / float(dim) * 2.0 - 1.0 + 1.0 / float(dim);
  float y = float(i / dim) / float(dim) * 2.0 - 1.0 + 1.0 / float(dim);
  gl_Position = vec4(x, y, 0.5, 1.0);
  gl_PointSize = pc.s.x > 0.0 ? pc.s.x : 1.0;
}
