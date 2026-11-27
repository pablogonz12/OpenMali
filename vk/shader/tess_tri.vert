#version 450
// Tessellation lane T1 (notes/tess-design.md): rot.vert, the VS of the tess pipeline. The levels are the TCS's (Vulkan
// has no VS-written tess levels), so this shader is byte-for-byte the plain scene VS: the tess pipeline's picture must
// equal the plain one.
layout(location=0) in vec4 pos;
layout(location=1) in vec4 col;
layout(location=0) out vec4 v_col;
layout(push_constant) uniform P { vec4 rot; } pc;
void main(){
  vec2 p = vec2(pos.x * pc.rot.x - pos.y * pc.rot.y, pos.x * pc.rot.y + pos.y * pc.rot.x) + pc.rot.zw;
  gl_Position = vec4(p, 0.5, 1.0);
  v_col = col;
}
