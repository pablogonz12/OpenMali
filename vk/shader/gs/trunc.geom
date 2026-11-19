#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec3 in_col[];
layout(location = 0) out vec3 out_col;
void main() {
   for (int i = 0; i < 3; i++) {
      gl_Position = gl_in[i].gl_Position;
      out_col = in_col[i];
      EmitVertex();
   }
   /* a fourth vertex beyond max_vertices must be dropped, not spill into the next invocation's slot */
   gl_Position = vec4(9.0, 9.0, 0.0, 1.0);
   out_col = vec3(1.0);
   EmitVertex();
   EndPrimitive();
}
