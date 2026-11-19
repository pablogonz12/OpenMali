#version 450
/* triangles in: emits a quad only for a triangle whose first vertex is on the right (the second primitive of
 * the test): the restart-padded draw count must still cover its slot. */
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec3 in_col[];
layout(location = 0) out vec3 out_col;
void main() {
   if (gl_in[0].gl_Position.x <= 0.0)
      return;
   vec4 c = gl_in[0].gl_Position;
   float s = 0.25;
   gl_Position = c + vec4(-s, -s, 0.0, 0.0); out_col = in_col[0]; EmitVertex();
   gl_Position = c + vec4( s, -s, 0.0, 0.0); out_col = in_col[0]; EmitVertex();
   gl_Position = c + vec4(-s,  s, 0.0, 0.0); out_col = in_col[0]; EmitVertex();
   gl_Position = c + vec4( s,  s, 0.0, 0.0); out_col = in_col[0]; EmitVertex();
   EndPrimitive();
}
