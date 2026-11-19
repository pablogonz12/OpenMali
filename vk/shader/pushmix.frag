#version 460
layout(location = 0) out vec4 fo;
layout(location = 0) flat in int idx;
layout(push_constant) uniform PC { vec4 a[40]; } pc;
void main() { fo = pc.a[idx] + pc.a[39]; }
