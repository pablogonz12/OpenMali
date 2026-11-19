#version 450
// gl_PointSize written under a TRIANGLE_LIST pipeline (DXVK / Unity do this): the driver strips PSIZ (mali_compile.c strip_psiz).
layout(location=0) in vec4 pos;
layout(location=1) in vec4 col;
layout(location=0) out vec4 v_col;
layout(push_constant) uniform P { vec4 rot; } pc;
out gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[1]; };   // ClipDistance makes the driver split the block into one variable per member, so PSIZ is its own variable
void main(){
  vec2 p = vec2(pos.x * pc.rot.x - pos.y * pc.rot.y, pos.x * pc.rot.y + pos.y * pc.rot.x) + pc.rot.zw;
  gl_Position = vec4(p, 0.5, 1.0);
  gl_PointSize = 1.0;
  gl_ClipDistance[0] = 1.0;
  v_col = col;
}
