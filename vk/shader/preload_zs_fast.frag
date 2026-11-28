#version 450
#extension GL_ARB_shader_stencil_export : require
// Specialised depth (+ stencil) loadOp LOAD: preload_zs.frag with the depth format fixed at build time instead of a
// per-pixel switch on p.zfmt. Variants (gen-preload-fast-spv.sh): -DZ32 (D32_SFLOAT), -DZ16 (D16_UNORM), -DZ24 (D24 in the
// low bits of a word); -DDO_S adds the stencil plane (binding 1); -DOUT_SAMPLE reads gl_SampleID's plane. Same push block
// and bindings as preload_zs.frag; the depth and stencil expressions are preload_zs.frag's, so the values are identical.
// Stencil-only LOAD keeps preload_zs.frag -DDO_S (it has no format switch).
layout(set = 0, binding = 0, std430) readonly buffer Z { uint d[]; } zs;
#ifdef DO_S
layout(set = 0, binding = 1, std430) readonly buffer St { uint d[]; } ss;
#endif
layout(push_constant) uniform P { uint zrow; uint srow; uint zfmt; uint zstride; uint sstride; } p;
void main() {
   uvec2 f = uvec2(gl_FragCoord.xy);
#ifdef OUT_SAMPLE
   uint zso = uint(gl_SampleID) * p.zstride;
   uint sso = uint(gl_SampleID) * p.sstride;
#else
   const uint zso = 0u, sso = 0u;
#endif
#ifdef Z16
   uint off = f.y * p.zrow + f.x * 2u + zso;
   gl_FragDepth = float((zs.d[off >> 2] >> ((off & 3u) * 8u)) & 0xffffu) / 65535.0;
#else
   uint w = zs.d[(f.y * p.zrow + f.x * 4u + zso) >> 2];
#ifdef Z24
   gl_FragDepth = float(w & 0xffffffu) / 16777215.0;
#else
   gl_FragDepth = uintBitsToFloat(w);
#endif
#endif
#ifdef DO_S
   uint soff = f.y * p.srow + f.x + sso;
   gl_FragStencilRefARB = int((ss.d[soff >> 2] >> ((soff & 3u) * 8u)) & 0xffu);
#endif
}
