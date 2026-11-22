#version 450
// tess_reject: the pass-through, but every vertex is moved — at level 1 the vertex stream would differ, so T1 refuses it.
layout(triangles, equal_spacing, ccw) in;
layout(location=0) in vec4 v_col[];
layout(location=0) out vec4 f_col;
void main(){
  vec3 c = gl_TessCoord;
  int i = (c.y > 0.5) ? 1 : ((c.z > 0.5) ? 2 : 0);
  gl_Position = gl_in[i].gl_Position + vec4(0.25, 0.0, 0.0, 0.0);
  f_col = v_col[i];
}
