#version 450
/* flat varying: dFdx must read 0.0 everywhere. */
layout(location = 0) in vec3 v0;
layout(location = 1) flat in vec3 v1;
layout(location = 0) out vec4 o;
void main() { o = vec4(v1.x, dFdx(v1.x) * 64.0, 0.0, 0.0); }
