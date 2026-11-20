#version 450
/* Draw-submission benchmark: one tiny corner triangle per draw, so the frame is not fill
 * bound. The per-draw dynamic uniform offset shifts it by a sub-pixel amount, which stops
 * consecutive draws from being merged. */
layout(binding = 0) uniform Off { vec4 off; } u;

void main()
{
   vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
   gl_Position = vec4(p * 0.02 - 1.0 + u.off.xy * 0.0001, 0.0, 1.0);
}
