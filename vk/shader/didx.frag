#version 450
#extension GL_EXT_nonuniform_qualifier : require
// R4-B2 descriptor indexing: runtime array of sampled images, divergent (non-uniform) index per pixel of a 2x2 quad.
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform texture2D tex[];
layout(set=0, binding=1) uniform sampler smp;
void main(){
  int i = (int(gl_FragCoord.x) & 1) + 2 * (int(gl_FragCoord.y) & 1);
  o = texture(sampler2D(tex[nonuniformEXT(i)], smp), vec2(0.5));
}
