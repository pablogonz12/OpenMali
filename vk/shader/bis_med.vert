#version 450
/* Oversized triangle, location 0 declared mediump in both stages. */
layout(location = 0) out mediump vec3 v0;
void main()
{
   vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
   mediump vec2 q = p * 0.5;
   v0 = vec3(q, 0.25);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
