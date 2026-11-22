#version 450
/* Format sweep, texture half: fetch one known texel of a known 8x8/4x4/2x2 mip chain and write it to the colour
 * target. texelFetch returns the raw texel (no sRGB decode, no filtering); textureLod() at the texel centre with a
 * NEAREST/CLAMP sampler returns the filtered value, which for an sRGB format IS linearised. One file, three classes:
 * -DIS_UINT/DIS_SINT for the integer sampled formats (texelFetch only: they cannot be filtered). */
#ifdef IS_UINT
layout(set = 0, binding = 0) uniform usampler2D t;
#elif defined(IS_SINT)
layout(set = 0, binding = 0) uniform isampler2D t;
#else
layout(set = 0, binding = 0) uniform sampler2D t;
#endif
layout(push_constant) uniform PC { vec4 scale; vec4 bias; ivec4 c; } pc;   /* c = x, y, lod, mode */
layout(location = 0) out vec4 o;
void main() {
   ivec2 xy = pc.c.xy;
   int lod = pc.c.z;
   vec2 uv = (vec2(xy) + 0.5) / 8.0;
#ifdef IS_UINT
   o = vec4(vec3(texelFetch(t, xy, lod).rgb) / 255.0, 1.0);
#elif defined(IS_SINT)
   o = vec4((vec3(texelFetch(t, xy, lod).rgb) + 128.0) / 255.0, 1.0);
#else
   vec3 r = pc.c.w == 0 ? texelFetch(t, xy, lod).rgb : textureLod(t, uv, float(lod)).rgb;
   o = vec4(r * pc.scale.rgb + pc.bias.rgb, 1.0);
#endif
}
