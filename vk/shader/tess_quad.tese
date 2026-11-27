#version 450
// tess_reject: quads are a T2 shape (a quad patch is not a triangle patch); T1 accepts triangles only.
layout(quads, equal_spacing, ccw) in;
layout(location=0) in vec4 v_col[];
layout(location=0) out vec4 f_col;
void main(){
  gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position
              + gl_TessCoord.z * gl_in[2].gl_Position;
  f_col = v_col[0];
}
