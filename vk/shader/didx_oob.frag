#version 450
#extension GL_EXT_nonuniform_qualifier : require
// R4-B2 descriptor indexing: partially-bound sampled-image array. Index 0..3 are written, 4..7 are not, and one band of
// columns uses 64 (past the layout count). Both the unwritten slots and the out-of-range index must read the Null Descriptor
// (zero), not garbage from the next binding or past the set.
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform texture2D tex[];
layout(set=0, binding=1) uniform sampler smp;
void main(){
  int i = (int(gl_FragCoord.x) & 1) + 2 * (int(gl_FragCoord.y) & 1);
  if ((int(gl_FragCoord.x) & 2) != 0) i = 64;
  vec4 c = texture(sampler2D(tex[nonuniformEXT(i)], smp), vec2(0.5));
  o = vec4(c.rgb, 1.0);
}
