#version 450
// R4-B2 shaderClipDistance: full-screen quad; clip distance 0 = x (keep x >= 0), clip distance 2 = y - 0.5 (keep upper part; index 1 stays 1).
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[3]; };
const vec2 p[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
void main(){
  vec2 q = p[gl_VertexIndex];
  vec2 n = q * 2.0 - 1.0;
  gl_Position = vec4(n, 0.5, 1.0);
  gl_ClipDistance[0] = n.x;
  gl_ClipDistance[1] = 1.0;
  gl_ClipDistance[2] = n.y - 0.5;
}
