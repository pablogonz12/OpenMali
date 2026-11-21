#version 450
// vary16-own: the 8-byte twin (two components in one slot) — the same draw, the same frame, one variable.
layout(location = 0) out vec2 v;
void main(){
   vec2 p = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0);
   gl_Position = vec4(p, 0.5, 1.0);
   v = vec2(0.50 + 0.125 * p.x, 0.50 + 0.125 * p.y);
}
