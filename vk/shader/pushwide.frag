#version 460
layout(location = 0) out vec4 fo;
layout(push_constant) uniform PC { vec4 a[8]; } pc;
void main() { fo = pc.a[0] + pc.a[7]; }
