#version 450
// R4 fixed-function tests: like rot.vert but z comes from the push constant (rot.w); rot.xy = (cos, sin) with offset zero here.
layout(location=0) in vec4 pos;
layout(location=1) in vec4 col;
layout(location=0) out vec4 v_col;
layout(push_constant) uniform P { vec4 rot; } pc;
void main(){
  gl_Position = vec4(pos.xy * pc.rot.x, pc.rot.w, 1.0);
  v_col = col;
}
