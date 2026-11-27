#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o;
layout(push_constant) uniform P { vec4 s; vec4 i; } pc;
layout(set=0, binding=0) uniform texture2D tex[];
layout(set=0, binding=1) uniform sampler smp;
void main(){ o = texture(nonuniformEXT(sampler2D(tex[int(pc.i.x)], smp)), vec2(0.5)); }
