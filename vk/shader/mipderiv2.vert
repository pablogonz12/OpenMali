#version 450
/* Full-screen triangle with a smooth and a flat copy of the same value (t_vk_mip --deriv2). */
layout(location = 0) out vec2 vUV;
layout(location = 1) flat out float flatv;
void main()
{
   vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
   vUV = p * 0.5;
   flatv = 0.5;
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
