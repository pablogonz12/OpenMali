#version 450
/* triangles in -> one quad (triangle strip) out, centred on the input triangle's centroid */
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in vec3 in_col[];
layout(location = 0) out vec3 out_col;
void main() {
   vec4 c = (gl_in[0].gl_Position + gl_in[1].gl_Position + gl_in[2].gl_Position) / 3.0;
   float s = 0.3;
   gl_Position = c + vec4(-s, -s, 0.0, 0.0); out_col = in_col[0]; EmitVertex();
   gl_Position = c + vec4( s, -s, 0.0, 0.0); out_col = in_col[0]; EmitVertex();
   gl_Position = c + vec4(-s,  s, 0.0, 0.0); out_col = in_col[0]; EmitVertex();
   gl_Position = c + vec4( s,  s, 0.0, 0.0); out_col = in_col[0]; EmitVertex();
   EndPrimitive();
}
