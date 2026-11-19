#version 450
// MALI_ADVERTISE_CULL shim: full-screen quad whose gl_CullDistance[0] is -1 at every vertex (a conformant driver culls it all). The shim IGNORES
// cull distances, so the quad must still be drawn. gl_ClipDistance[0] = 1 (keep) proves clip and cull coexist in one gl_PerVertex.
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[1]; float gl_CullDistance[2]; };
const vec2 p[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
void main(){
  vec2 n = p[gl_VertexIndex] * 2.0 - 1.0;
  gl_Position = vec4(n, 0.5, 1.0);
  gl_ClipDistance[0] = 1.0;
  gl_CullDistance[0] = -1.0;
  gl_CullDistance[1] = -1.0;
}
