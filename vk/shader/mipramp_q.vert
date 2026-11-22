#version 450
/* Two triangles covering exactly the target, vUV = (pos+1)/2 = pixel/(RT) exactly (t_vk_mip --ramp).
 * The oversized triangle used before maps vUV 0..1 over NDC -1..3, i.e. only 0..0.5 on screen: that is what
 * made the interpolated values look halved. */
layout(location = 0) out vec2 vUV;
void main()
{
   const vec2 c[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
   vec2 p = c[gl_VertexIndex];
   vUV = (p + 1.0) * 0.5;
   gl_Position = vec4(p, 0.0, 1.0);
}
