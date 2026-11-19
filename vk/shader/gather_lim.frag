#version 450
#extension GL_ARB_gpu_shader5 : enable
// Gather offsets at the advertised minTexelGatherOffset (-8) / maxTexelGatherOffset (7), constant (narrow i4 path) and run-time (i8 register path).
// 32x32 RG8 texture T(x,y) = (8x, 8y). Gather at the texel corner (16,16) -> footprint base (15,15); component w = texel (15,15)+offset.
// Quadrants: 0 const (-8,-8); 1 const (7,7); 2 run-time (-8,7); 3 run-time (7,-8). Output: r = x of texel w, g = y of texel w.
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform sampler2D tex;
void main(){
  int q = (v_uv.x >= 0.5 ? 1 : 0) + (v_uv.y >= 0.5 ? 2 : 0);
  ivec2 rt = (q == 2) ? ivec2(-8, 7) : ivec2(7, -8);
  vec4 gx, gy;
  if (q == 0) { gx = textureGatherOffset(tex, vec2(0.5), ivec2(-8, -8), 0); gy = textureGatherOffset(tex, vec2(0.5), ivec2(-8, -8), 1); }
  else if (q == 1) { gx = textureGatherOffset(tex, vec2(0.5), ivec2(7, 7), 0); gy = textureGatherOffset(tex, vec2(0.5), ivec2(7, 7), 1); }
  else { gx = textureGatherOffset(tex, vec2(0.5), rt, 0); gy = textureGatherOffset(tex, vec2(0.5), rt, 1); }
  o = vec4(gx.w, gy.w, 0.0, 1.0);
}
