#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(location=0) in flat int k;
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform texture2D tex[];
layout(set=0, binding=1) uniform sampler smp;
void main(){ o = texture(nonuniformEXT(sampler2D(tex[k], smp)), vec2(0.5)); }
