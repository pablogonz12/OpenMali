#version 450
/* EXPLICIT LOD, SAME NUMBER: the analytic LOD of mipramp.frag, handed to textureLod. If this twin passes and the
 * implicit one fails, the level selection is fine and only the implicit path's derivative is wrong. */
layout(push_constant) uniform PC { float lod; float scale; } pc;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o;
void main()
{
   float rho = max(2.0 * vUV.x * pc.scale, 1e-6);   /* |du/dx| * N with N = RT = 64 */
   o = textureLod(tex, vec2(vUV.x * vUV.x * pc.scale, 0.5), log2(rho));
}
