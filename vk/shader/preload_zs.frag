#version 450
#extension GL_ARB_shader_stencil_export : require
// depth/stencil loadOp LOAD: re-emits the old depth (D32F words / D16 halves) and/or stencil (bytes) through the fragment depth/stencil output.
// Build with -DDO_Z and/or -DDO_S. Depth is binding 0; stencil is binding 0 (stencil only) or 1.
#ifdef DO_Z
layout(set = 0, binding = 0, std430) readonly buffer Z { uint d[]; } zs;
#endif
#ifdef DO_S
#ifdef DO_Z
layout(set = 0, binding = 1, std430) readonly buffer St { uint d[]; } ss;
#else
layout(set = 0, binding = 0, std430) readonly buffer St { uint d[]; } ss;
#endif
#endif
layout(push_constant) uniform P { uint zrow; uint srow; uint zfmt; uint zstride; uint sstride; } p;
// -DOUT_SAMPLE: multisampled loadOp LOAD (zink/CEF start a 4x pass with stencilLoadOp LOAD). The pass has N samples and the fragment
// shader runs once per sample (sample shading, DCD0 evaluate-per-sample), so each invocation reads *its own* sample plane: pad carries
// the bytes per sample plane (the surface stride) and gl_SampleID picks it. Same model as the multisampled colour preload (preload.frag).
#ifdef OUT_SAMPLE
#define Z_SAMPLE_OFF (uint(gl_SampleID) * p.zstride)
#define S_SAMPLE_OFF (uint(gl_SampleID) * p.sstride)
#else
#define Z_SAMPLE_OFF 0u
#define S_SAMPLE_OFF 0u
#endif
void main() {
   uvec2 f = uvec2(gl_FragCoord.xy);
#ifdef DO_Z
#ifdef OUT_SAMPLE
   uint zoff = f.y * p.zrow + f.x * (p.zfmt == 1u ? 2u : 4u) + Z_SAMPLE_OFF;
   if (p.zfmt == 0u) {
      gl_FragDepth = uintBitsToFloat(zs.d[zoff >> 2]);
   } else if (p.zfmt == 2u) {
      gl_FragDepth = float(zs.d[zoff >> 2] & 0xffffffu) / 16777215.0;
   } else {
      gl_FragDepth = float((zs.d[zoff >> 2] >> ((zoff & 3u) * 8u)) & 0xffffu) / 65535.0;
   }
#else
   if (p.zfmt == 0u) {
      gl_FragDepth = uintBitsToFloat(zs.d[(f.y * p.zrow >> 2) + f.x]);
   } else if (p.zfmt == 2u) {   // D24_UNORM_S8_UINT / D24X8: 24-bit UNORM in the low bits of a 32-bit word (stencil in the top byte)
      gl_FragDepth = float(zs.d[(f.y * p.zrow >> 2) + f.x] & 0xffffffu) / 16777215.0;
   } else {
      uint off = f.y * p.zrow + f.x * 2u;
      gl_FragDepth = float((zs.d[off >> 2] >> ((off & 3u) * 8u)) & 0xffffu) / 65535.0;
   }
#endif
#endif
#ifdef DO_S
#ifdef OUT_SAMPLE
   uint soff = f.y * p.srow + f.x + S_SAMPLE_OFF;
#else
   uint soff = f.y * p.srow + f.x;
#endif
   gl_FragStencilRefARB = int((ss.d[soff >> 2] >> ((soff & 3u) * 8u)) & 0xffu);
#endif
}
