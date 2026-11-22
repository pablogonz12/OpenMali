#version 450
/* Oversized full-screen triangle, 3 varyings, value = (pos+1)/2 at each vertex (t_vk_mip --bisect). */
layout(location = 0) out vec3 v0;
layout(location = 1) out vec3 v1;
layout(location = 2) out vec3 v2;
void main()
{
   vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
   vec2 uv = p * 0.5;
   v0 = vec3(uv, 0.25);
   v1 = vec3(uv, 0.5);
   v2 = vec3(uv, 0.75);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
