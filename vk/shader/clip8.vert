#version 450
// maxClipDistances = 8: full-screen quad writing all 8 distances; [0] = x (keep x >= 0), [7] = y - 0.5 (keep upper part), [1..6] stay 1.
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[8]; };
const vec2 p[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
void main(){
  vec2 n = p[gl_VertexIndex] * 2.0 - 1.0;
  gl_Position = vec4(n, 0.5, 1.0);
  for (int i = 1; i < 7; i++) gl_ClipDistance[i] = 1.0;
  gl_ClipDistance[0] = n.x;
  gl_ClipDistance[7] = n.y - 0.5;
}
