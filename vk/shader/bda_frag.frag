#version 460
#extension GL_EXT_buffer_reference : require
layout(location=0) out vec4 fo;
layout(buffer_reference, std430) buffer BR { uint x; };
layout(push_constant) uniform PC { BR p; } pc;
void main(){ pc.p.x = 2u; fo=vec4(1); }
