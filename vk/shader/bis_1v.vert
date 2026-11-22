#version 450
/* Oversized triangle, ONE varying at location 0. */
layout(location = 0) out vec3 v0;
void main()
{
   vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
   v0 = vec3(p * 0.5, 0.25);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
