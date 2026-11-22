#version 450
// T1's accepted TES: the pass-through *select*. At gl_TessCoord = (1,0,0) / (0,1,0) / (0,0,1) — the three output vertices
// of a level-1 triangle patch — the selector picks patch control point 0 / 1 / 2 and copies it unchanged. mali_tess.c
// proves that by evaluating the body at those three coordinates and checking the result is an identity copy (the note
// forbids pattern-matching the arithmetic, so it is folded, not matched). The barycentric form
// (p0*c.x + p1*c.y + p2*c.z) is *not* accepted: `x * 0` does not fold away under Vulkan's float rules, so the result is
// not provably a copy — real barycentric TESs need T2's tessellator, which is exactly the split this lane draws.
layout(triangles, equal_spacing, ccw) in;
layout(location=0) in vec4 v_col[];
layout(location=0) out vec4 f_col;
void main(){
  /* No local: gl_TessCoord is read straight into the index expression, so the classifier's constant substitute lands in
   * the arithmetic itself (a `vec3 c = gl_TessCoord;` local goes through a temporary the fold cannot see through). */
  int i = int(gl_TessCoord.y + 2.0 * gl_TessCoord.z + 0.5);   /* e0 -> 0, e1 -> 1, e2 -> 2 */
  gl_Position = gl_in[i].gl_Position + vec4(0.25, 0.0, 0.0, 0.0);
  f_col = v_col[i];
}
