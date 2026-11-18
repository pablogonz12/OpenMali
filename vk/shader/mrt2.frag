#version 450
// two colour outputs, a distinct constant colour each
layout(location=0) out vec4 o0;
layout(location=1) out vec4 o1;
void main(){ o0 = vec4(1, 0, 0, 1); o1 = vec4(0, 1, 0, 1); }
