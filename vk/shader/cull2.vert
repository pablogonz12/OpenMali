#version 450
// shaderCullDistance on the SECOND flag slot: 3 clip + 5 cull distances (the 8-component limit). Cull distances 0..3 are
// always +1, only cull[4] (component 0 of the second slot) decides, so a lowering that mishandles the slot index
// (counts it twice, reads the first slot's flags, takes the FS base from num_inputs) draws the wrong columns or makes
// the VS and FS disagree and the pipeline is refused. Thirds as in cull.vert:
//   A: cull[4] = -1 at all three vertices -> vanishes     B: cull[4] = -1 at ONE vertex -> kept whole
//   C: cull[4] = +1 everywhere            -> kept whole (clip[0..2] = +1: nothing clipped)
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[3]; float gl_CullDistance[5]; };
const vec2 p[18] = vec2[18](
   vec2(0.0,     0.0), vec2(0.0,     1.0), vec2(1.0/3.0, 0.0),
   vec2(0.0,     1.0), vec2(1.0/3.0, 1.0), vec2(1.0/3.0, 0.0),
   vec2(1.0/3.0, 0.0), vec2(1.0/3.0, 1.0), vec2(2.0/3.0, 0.0),
   vec2(1.0/3.0, 1.0), vec2(2.0/3.0, 1.0), vec2(2.0/3.0, 0.0),
   vec2(2.0/3.0, 0.0), vec2(2.0/3.0, 1.0), vec2(1.0,     0.0),
   vec2(2.0/3.0, 1.0), vec2(1.0,     1.0), vec2(1.0,     0.0));
void main(){
  gl_Position = vec4(p[gl_VertexIndex] * 2.0 - 1.0, 0.5, 1.0);
  for (int i = 0; i < 3; i++) gl_ClipDistance[i] = 1.0;
  for (int i = 0; i < 4; i++) gl_CullDistance[i] = 1.0;
  if (gl_VertexIndex < 6) gl_CullDistance[4] = -1.0;
  else if (gl_VertexIndex < 12) gl_CullDistance[4] = gl_VertexIndex == 6 ? -1.0 : 1.0;
  else gl_CullDistance[4] = 1.0;
}
