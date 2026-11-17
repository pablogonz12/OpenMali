#version 450
// M9 rest: full-screen-style quad, uv = colour attribute xy, push constant rot = (cos, sin, offset.xy) like rot.vert.
layout(location=0) in vec4 pos;
layout(location=1) in vec4 col;
layout(location=0) out vec2 v_uv;
layout(push_constant) uniform P { vec4 rot; } pc;
void main(){
  vec2 p = vec2(pos.x * pc.rot.x - pos.y * pc.rot.y, pos.x * pc.rot.y + pos.y * pc.rot.x) + pc.rot.zw;
  gl_Position = vec4(p, 0.5, 1.0);
  v_uv = col.xy;
}
