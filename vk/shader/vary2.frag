#version 450
// vary16-own: the 8-byte twin consumes both components; the packed constant makes its readback differ from vary4.
layout(location = 0) in vec2 v;
layout(location = 0) out vec4 out_color;
void main(){ out_color = vec4(v, 0.10, 0.90); }
