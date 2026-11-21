#version 450
// vary16-own: consumes all four components of the 16-byte varying.
layout(location = 0) in vec4 v;
layout(location = 0) out vec4 out_color;
void main(){ out_color = v; }
