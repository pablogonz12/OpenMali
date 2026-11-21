#version 450
// vary16-own: the 16-byte twin. One vec4 output at location 0, all four components different per vertex and
// varying across the frame, so the FS cannot read a constant and the readback shows the interpolation.
layout(location = 0) out vec4 v;
void main(){
   vec2 p = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0);
   gl_Position = vec4(p, 0.5, 1.0);
   v = vec4(0.50 + 0.125 * p.x, 0.50 + 0.125 * p.y, 0.25 + 0.125 * p.x, 0.75 + 0.100 * p.y);
}
