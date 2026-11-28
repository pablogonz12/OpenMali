#version 450
// shaderCullDistance exactness edges (W = H = 256). One cull distance, 12 vertices:
//   Q1 (x in [0, W/4)):  T1 has two NEGATIVE vertices on its top edge y = 128.5 (a pixel-centre row) and a positive one
//                        below; T2 (one negative vertex) fills the rest of the rectangle [128.5, 256). Both are kept.
//                        On row 128 the interpolated flag of T1 is exactly 0 (the positive vertex has weight 0 there),
//                        so a value-only FS test drops that row; the value + derivative test keeps it.
//   Q2 (x in [W/4, W/2)): the same geometry with every distance +1: the control for which row-128 samples the
//                        rasteriser's tie-break rule covers. Q1 must equal Q2 pixel for pixel.
//   R  (x in [W/2, W)):  full height, distances exactly +0.0 (R1) and -0.0 (R2): zero is not negative -> drawn.
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[1]; };
const float yc = 0.00390625;                 /* NDC of pixel-centre row 128.5 for H = 256, exact in binary */
const vec2 p[12] = vec2[12](
   vec2(-1.0, yc), vec2(-0.5, yc), vec2(-1.0, 1.0),     /* T1: N N P (top edge between the two negatives) */
   vec2(-0.5, yc), vec2(-0.5, 1.0), vec2(-1.0, 1.0),    /* T2: N P P */
   vec2(-0.5, yc), vec2( 0.0, yc), vec2(-0.5, 1.0),     /* control T1 */
   vec2( 0.0, yc), vec2( 0.0, 1.0), vec2(-0.5, 1.0));   /* control T2 */
const float d[12] = float[12](-1.0, -1.0, 1.0,  -1.0, 1.0, 1.0,  1.0, 1.0, 1.0,  1.0, 1.0, 1.0);
void main(){
  int i = gl_VertexIndex;
  if (i < 12) {
    gl_Position = vec4(p[i], 0.5, 1.0);
    gl_CullDistance[0] = d[i];
  } else {                                     /* 12..14 = R1 (+0.0), 15..17 = R2 (-0.0) */
    const vec2 r[6] = vec2[6](vec2(0.0, -1.0), vec2(0.0, 1.0), vec2(1.0, -1.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    gl_Position = vec4(r[i - 12], 0.5, 1.0);
    gl_CullDistance[0] = i < 15 ? 0.0 : -0.0;
  }
}
