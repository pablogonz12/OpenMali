#version 450
// gs_reject: 1-in/1-out and in order, but the position is *computed* (a small offset), so the vertex shader alone cannot
// reproduce it. Refused: "an output is computed, not copied from the input vertex" -- accepting it would misrender.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 in_col[];
layout(location = 0) out vec4 v_col;
void main(){
  gl_Position = gl_in[0].gl_Position + vec4(0.1, 0.0, 0.0, 0.0); v_col = in_col[0]; EmitVertex();
  gl_Position = gl_in[1].gl_Position + vec4(0.1, 0.0, 0.0, 0.0); v_col = in_col[1]; EmitVertex();
  gl_Position = gl_in[2].gl_Position + vec4(0.1, 0.0, 0.0, 0.0); v_col = in_col[2]; EmitVertex();
  EndPrimitive();
}
