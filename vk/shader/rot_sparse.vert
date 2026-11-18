#version 450
// sparse attribute locations 0 and 2 (layout_skip); R2 t_compiled: rotated/offset triangle with per-vertex colour. Push constant (VS FAU bytes 32..): rot = (cos, sin, offset.x, offset.y).
layout(location=0) in vec4 pos;
layout(location=2) in vec4 col;
layout(location=0) out vec4 v_col;
layout(push_constant) uniform P { vec4 rot; } pc;
void main(){
  vec2 p = vec2(pos.x * pc.rot.x - pos.y * pc.rot.y, pos.x * pc.rot.y + pos.y * pc.rot.x) + pc.rot.zw;
  gl_Position = vec4(p, 0.5, 1.0);
  v_col = col;
}
