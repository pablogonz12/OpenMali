#version 450
/* GS lane 2, step (ii): a real geometry shader — two triangles out per triangle in, the second shifted down half a
 * screen. Nothing about it is a pass-through: it emits 6 vertices for 3, so lane 1's classifier must refuse it (it does)
 * and only the compute pre-pass can draw it. Vertices: the scene's 3-vertex triangle with a per-vertex colour. */
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
layout(location = 0) in vec4 in_col[];
layout(location = 0) out vec4 v_col;
void main()
{
   for (int i = 0; i < 3; i++) {
      gl_Position = gl_in[i].gl_Position;
      v_col = in_col[i];
      EmitVertex();
   }
   EndPrimitive();
   for (int i = 0; i < 3; i++) {
      gl_Position = gl_in[i].gl_Position * vec4(0.5, 0.5, 1.0, 1.0) + vec4(0.0, -0.5, 0.0, 0.0);
      v_col = in_col[i];
      EmitVertex();
   }
   EndPrimitive();
}
