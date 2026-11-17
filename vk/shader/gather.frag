#version 450
#extension GL_ARB_gpu_shader5 : enable
// R4-B2 shaderImageGatherExtended: quadrants 0..2 gather with a run-time offset (q&1, q>>1), quadrant 3 uses textureGatherOffsets.
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform sampler2D tex;
void main(){
  int q = (v_uv.x >= 0.5 ? 1 : 0) + (v_uv.y >= 0.5 ? 2 : 0);
  if (q < 3) o = textureGatherOffset(tex, vec2(0.5), ivec2(q & 1, q >> 1), 0);
  else o = textureGatherOffsets(tex, vec2(0.5), ivec2[4](ivec2(0,0), ivec2(1,0), ivec2(0,1), ivec2(1,1)), 0);
}
