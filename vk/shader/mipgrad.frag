#version 450
/* GRADIENT EXPLICIT, ANALYTIC: same coordinate as mipramp.frag, but the gradient is written out in closed form
 * (no derivative instructions), so the sampler's grad path is exercised with values the CPU can predict. */
layout(push_constant) uniform PC { float lod; float scale; } pc;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o;
void main()
{
   vec2 uv = vec2(vUV.x * vUV.x * pc.scale, 0.5);
   float dudx = 2.0 * vUV.x * pc.scale / 64.0;   /* 64 = render target width */
   o = textureGrad(tex, uv, vec2(dudx, 0.0), vec2(0.0, 0.0));
}
