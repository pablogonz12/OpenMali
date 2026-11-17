#version 450
// Array texture with mips: four quadrants sample (layer 1, mip 0), (layer 2, mip 1), (layer 1, mip 2), (layer 2, mip 3) of a sampler2DArray.
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform sampler2DArray tex;
void main(){
  int q = (v_uv.x >= 0.5 ? 1 : 0) + (v_uv.y >= 0.5 ? 2 : 0);
  float layer = (q == 0 || q == 2) ? 1.0 : 2.0;
  float lod = float(q);
  o = textureLod(tex, vec3(fract(v_uv * 2.0), layer), lod);
}
