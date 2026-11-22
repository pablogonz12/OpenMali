#version 450
// T1's accepted TES: at gl_TessCoord = (1,0,0) / (0,1,0) / (0,0,1) the output is patch control point 0 / 1 / 2, copied
// unchanged (the selector folds to a constant, which is what mali_tess.c checks after Mesa evaluates it — the note forbids
// pattern-matching the arithmetic).
layout(triangles, equal_spacing, ccw) in;
layout(location=0) in vec4 v_col[];
layout(location=0) out vec4 f_col;
void main(){
  vec3 c = gl_TessCoord;
  int i = (c.y > 0.5) ? 1 : ((c.z > 0.5) ? 2 : 0);
  gl_Position = gl_in[i].gl_Position;
  f_col = v_col[i];
}
