#version 450
// Specialised colour loadOp LOAD (MALI_PRELOAD unset/hw/fastdraw; MALI_PRELOAD=draw keeps preload.frag for everything).
// One variant per (format class, sRGB or not, sample count), built by gen-preload-fast-spv.sh:
//   -DFMT_RGBA8 (R8G8B8A8 / A8B8G8R8 UNORM, preload.frag kind 0 with 4 bytes), -DFMT_BGRA8 (kind 1),
//   -DFMT_A2B10G10R10 (kind 4), -DFMT_A2R10G10B10 (kind 5); -DSRGB (RGBA8/BGRA8 only); -DOUT_SAMPLE (4x, per sample).
// Same push block and binding as preload.frag. Every texel is one aligned 32-bit word (row and plane strides are multiples
// of 16), so the address is a word index (texelFetch-style) and nothing per pixel depends on a branch. The value path is
// preload.frag's expression for the same kind, so what reaches the tile buffer is bit-identical; the sRGB decode is the same
// two expressions, selected per channel instead of branched.
layout(set = 0, binding = 0, std430) readonly buffer S { uint d[]; } src;
layout(push_constant) uniform P { uint row_bytes; uint flags; uint bpp; uint pad; } p;
layout(location = 0) out vec4 o;
void main() {
   uvec2 f = uvec2(gl_FragCoord.xy);
   uint i = f.y * (p.row_bytes >> 2) + f.x;
#ifdef OUT_SAMPLE
   i += (uint(gl_SampleID) * p.pad) >> 2;   // pad = bytes per sample plane
#endif
   uint w = src.d[i];
#if defined(FMT_RGBA8) || defined(FMT_BGRA8)
   vec4 c = unpackUnorm4x8(w);
#ifdef FMT_BGRA8
   c = c.bgra;
#endif
#else
   vec4 u = vec4(float(w & 1023u), float((w >> 10) & 1023u), float((w >> 20) & 1023u), float(w >> 30)) / vec4(1023.0, 1023.0, 1023.0, 3.0);
#ifdef FMT_A2R10G10B10
   vec4 c = vec4(u.z, u.y, u.x, u.w);
#else
   vec4 c = u;
#endif
#endif
#ifdef SRGB
   c.rgb = mix(pow((c.rgb + 0.055) / 1.055, vec3(2.4)), c.rgb / 12.92, lessThanEqual(c.rgb, vec3(0.04045)));
#endif
   o = c;
}
