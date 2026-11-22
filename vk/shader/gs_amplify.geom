#version 450
// gs_reject: one triangle in, six vertices out (a real amplification). Refused: "max_vertices 6 is not 3".
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
layout(location = 0) in vec4 in_col[];
layout(location = 0) out vec4 v_col;
void main(){
  for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; v_col = in_col[i]; EmitVertex(); }
  EndPrimitive();
  for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position * 0.5; v_col = in_col[i]; EmitVertex(); }
  EndPrimitive();
}
