#version 450
/* DERIVATIVE RATIO: red channel = dFdx(u) / analytic_du/dx * 0.25, so a correct derivative reads 64 and a
 * half-size one reads 32 (value/255*4 = ratio). Green/blue unused. */
layout(push_constant) uniform PC { float lod; float scale; } pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o;
void main()
{
   vec2 uv = vec2(vUV.x * vUV.x * pc.scale, 0.5);
   float an = 2.0 * vUV.x * pc.scale / 64.0;
   float r = (an > 1e-6) ? (dFdx(uv.x) / an) : 0.0;
   o = vec4(clamp(r * 0.25, 0.0, 1.0), 0.0, 0.0, 1.0);
}
