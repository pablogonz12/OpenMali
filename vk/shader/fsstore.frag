#version 450
// R4-B2 fragmentStoresAndAtomics: every covered pixel stores i+1 to an SSBO and a storage image and bumps an atomic counter.
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o;
layout(set=0, binding=0) buffer B { uint count; uint maxi; uint data[256*256]; } b;
layout(set=0, binding=1, r32ui) uniform writeonly uimage2D img;
void main(){
  uvec2 c = uvec2(gl_FragCoord.xy);
  uint i = c.y * 256u + c.x;
  b.data[i] = i + 1u;
  atomicAdd(b.count, 1u);
  atomicMax(b.maxi, i);
  imageStore(img, ivec2(c), uvec4(i + 1u));
  o = vec4(1.0, 0.0, 0.0, 1.0);
}
