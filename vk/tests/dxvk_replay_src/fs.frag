#version 450
layout(constant_id = 2) const int kMode = 0;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 outCol;
layout(set = 1, binding = 0) uniform sampler2D tex;
void main() {
  vec4 t = texture(tex, uv);
  if (kMode == 1) { if (t.a < 0.5) discard; }
  outCol = t * col;
}
