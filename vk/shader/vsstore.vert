#version 450
// vertexPipelineStoresAndAtomics (zink GL 4.2, and DXVK's VS-SSBO path): the vertex stage stores to an SSBO and uses two
// atomics, keeping the value each atomic returned so the host can check it (a store without the atomic results would pass
// even if atomics silently returned 0).
layout(set=0, binding=0) buffer B { uint count; uint maxi; uint data[8]; uint oldc[8]; } b;
layout(location=0) out vec4 o;
const vec2 p[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(-1,1), vec2(-1,1), vec2(1,-1), vec2(1,1));
void main(){
  uint i = uint(gl_VertexIndex);
  b.data[i] = i + 1u;
  b.oldc[i] = atomicAdd(b.count, 1u);
  atomicMax(b.maxi, i);
  gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
  o = vec4(1.0, 0.0, 0.0, 1.0);
}
