#version 450
// baseline + an integer flat varying (the CTS pattern), value from the push constant
layout(location=0) flat out int k;
layout(push_constant) uniform P { vec4 s; } pc;
void main(){
  int i = gl_VertexIndex, dim = int(pc.s.y + 0.5);
  gl_Position = vec4(float(i % dim) / float(dim) * 2.0 - 1.0 + 1.0 / float(dim),
                     float(i / dim) / float(dim) * 2.0 - 1.0 + 1.0 / float(dim), 0.5, 1.0);
  gl_PointSize = 1.0;
  k = i & 15;
}
