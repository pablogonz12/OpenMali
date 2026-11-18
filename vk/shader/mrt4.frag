#version 450
// four colour outputs, a distinct constant colour each
layout(location=0) out vec4 o0;
layout(location=1) out vec4 o1;
layout(location=2) out vec4 o2;
layout(location=3) out vec4 o3;
void main(){ o0 = vec4(1, 0, 0, 1); o1 = vec4(0, 1, 0, 1); o2 = vec4(0, 0, 1, 1); o3 = vec4(1, 1, 0, 1); }
