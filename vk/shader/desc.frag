#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 out_color;
layout(set=0, binding=0) uniform U { vec4 colour; } u;
layout(set=0, binding=1) uniform texture2D tex;
layout(set=0, binding=2) uniform sampler smp;
layout(push_constant) uniform P { vec4 bias; } p;
void main(){ out_color = texture(sampler2D(tex, smp), uv) * u.colour + p.bias; }
