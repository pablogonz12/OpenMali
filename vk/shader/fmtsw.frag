#version 450
/* Format sweep, vertex half: the attribute value arrives flat (one 16-byte varying, the size notes/vary16.md proved
 * works) and is written to the colour target after scale/bias. The scale/bias map the integer classes and the
 * USCALED/SSCALED values into [0,1] without hiding a wrong decode: the expected value is computed with the same
 * formula in tests/t_vk_draw.c. No clamp here: the 8-bit target does the clamping, as the reference does. */
layout(location = 0) flat in vec4 v;
layout(push_constant) uniform PC { vec4 scale; vec4 bias; ivec4 c; } pc;
layout(location = 0) out vec4 o;
void main() { o = vec4(v.rgb * pc.scale.rgb + pc.bias.rgb, 1.0); }
