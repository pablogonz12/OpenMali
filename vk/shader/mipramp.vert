#version 450
/* Full-screen triangle, vUV = screen position in [0,1] (t_vk_mip --ramp). */
layout(location = 0) out vec2 vUV;
void main()
{
   vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
   vUV = p * 0.5;
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
