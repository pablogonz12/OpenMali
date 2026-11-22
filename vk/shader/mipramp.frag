#version 450
/* IMPLICIT LOD: texture() on a coordinate whose gradient rises with x, so the level is a function of the column.
 * u = (x/RT)^2 * scale  ->  du/dx = 2*(x/RT)*scale/RT  ->  rho = 2*(x/RT)*scale*N/RT (N = RT here)  ->  lod = log2(rho). */
layout(push_constant) uniform PC { float lod; float scale; } pc;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, vec2(vUV.x * vUV.x * pc.scale, 0.5)); }
