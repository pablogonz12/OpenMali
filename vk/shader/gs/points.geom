#version 450
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) out vec2 out_uv;
void main() {
   vec4 p = gl_in[0].gl_Position;
   float s = 0.02;
   gl_Position = p + vec4(-s, -s, 0, 0); out_uv = vec2(0,0); EmitVertex();
   gl_Position = p + vec4( s, -s, 0, 0); out_uv = vec2(1,0); EmitVertex();
   gl_Position = p + vec4(-s,  s, 0, 0); out_uv = vec2(0,1); EmitVertex();
   gl_Position = p + vec4( s,  s, 0, 0); out_uv = vec2(1,1); EmitVertex();
   EndPrimitive();
}
