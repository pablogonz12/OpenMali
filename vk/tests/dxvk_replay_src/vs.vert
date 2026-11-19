#version 450
layout(constant_id = 0) const bool kFlip = false;
layout(constant_id = 1) const uint kNumColors = 1;
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inCol;
layout(location = 3) in vec4 inInst;   // binding 1, per-instance rate
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 col;
layout(set = 0, binding = 0, std140) uniform Cb { mat4 mvp; vec4 tint[4]; } cb;
void main() {
  vec4 p = cb.mvp * inPos;
  if (kFlip) p.y = -p.y;
  gl_Position = p;
  gl_PointSize = 1.0;   // required for POINT_LIST without maintenance5
  uv = inUv;
  col = inCol * inInst * cb.tint[gl_VertexIndex % kNumColors];
}
