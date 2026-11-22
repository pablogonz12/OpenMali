#version 450
/* GRADIENT EXPLICIT, FROM THE DERIVATIVE INSTRUCTIONS: same coordinate as mipramp.frag, gradient taken with
 * dFdx/dFdy. If this twin reproduces the implicit twin's one-level bias, the derivative instructions are the
 * problem; if it matches mipgrad.frag (analytic), the implicit-LOD rewrite is. */
layout(push_constant) uniform PC { float lod; float scale; } pc;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o;
void main()
{
   vec2 uv = vec2(vUV.x * vUV.x * pc.scale, 0.5);
   o = textureGrad(tex, uv, dFdx(uv), dFdy(uv));
}
