#version 450
// shaderCullDistance: one cull distance and one clip distance in the same vertex shader, three triangles that
// separate the cull rule ("discard the primitive only if EVERY vertex is negative") from the clip rule ("kill
// the fragments behind the plane"). Full-screen thirds, so the columns are x < W/3, W/3..2W/3, x > 2W/3.
//   A (left third):   cull[0] = -1 at all three vertices                      -> the primitive must vanish
//   B (middle third): cull[0] = -1 at ONE vertex, +1 at the other two         -> drawn whole, nothing clipped
//                     (a lowering that treats cull as clip loses ~half of it)
//   C (right third):  cull[0] = +1 everywhere, clip[0] = ndc.x - 0.5          -> ordinary clipping keeps x >= 0.75 W
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[1]; float gl_CullDistance[1]; };
const vec2 p[18] = vec2[18](
   vec2(0.0,     0.0), vec2(0.0,     1.0), vec2(1.0/3.0, 0.0),   /* A0 A1 A2 */
   vec2(0.0,     1.0), vec2(1.0/3.0, 1.0), vec2(1.0/3.0, 0.0),   /* A3 A4 A5 */
   vec2(1.0/3.0, 0.0), vec2(1.0/3.0, 1.0), vec2(2.0/3.0, 0.0),   /* B0 B1 B2 */
   vec2(1.0/3.0, 1.0), vec2(2.0/3.0, 1.0), vec2(2.0/3.0, 0.0),   /* B3 B4 B5 */
   vec2(2.0/3.0, 0.0), vec2(2.0/3.0, 1.0), vec2(1.0,     0.0),   /* C0 C1 C2 */
   vec2(2.0/3.0, 1.0), vec2(1.0,     1.0), vec2(1.0,     0.0));  /* C3 C4 C5 */
void main(){
  vec2 n = p[gl_VertexIndex] * 2.0 - 1.0;
  gl_Position = vec4(n, 0.5, 1.0);
  gl_ClipDistance[0] = 1.0;                       /* no clipping unless a triangle opts in */
  if (gl_VertexIndex < 6)                         /* A: all three vertices behind the cull plane */
    gl_CullDistance[0] = -1.0;
  else if (gl_VertexIndex < 12)                   /* B: only its first vertex is negative */
    gl_CullDistance[0] = gl_VertexIndex == 6 ? -1.0 : 1.0;
  else {
    gl_CullDistance[0] = 1.0;
    gl_ClipDistance[0] = n.x - 0.5;               /* C: clip keeps the right half of its column */
  }
}
