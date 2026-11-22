#version 450
/* location 0 declared mediump, like the vertex side */
layout(location = 0) in mediump vec3 v0;
layout(location = 0) out vec4 o;
void main() { mediump float u = v0.x; o = vec4(u, dFdx(u), 0.0, 0.0); }
