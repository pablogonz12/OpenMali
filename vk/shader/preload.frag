#version 450
// R4 colour loadOp LOAD: re-emits the framebuffer's old contents (linear RGBA8-family image seen as a buffer) into the tile.
layout(set = 0, binding = 0, std430) readonly buffer S { uint d[]; } src;
layout(push_constant) uniform P { uint row_words; uint flags; } p;   // flags: 1 = memory order is BGRA, 2 = sRGB
layout(location = 0) out vec4 o;
float dec(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
void main() {
   uvec2 f = uvec2(gl_FragCoord.xy);
   vec4 c = unpackUnorm4x8(src.d[f.y * p.row_words + f.x]);
   if ((p.flags & 1u) != 0u) c = c.bgra;
   if ((p.flags & 2u) != 0u) c.rgb = vec3(dec(c.r), dec(c.g), dec(c.b));
   o = c;
}
