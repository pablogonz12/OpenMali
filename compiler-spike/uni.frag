#version 450
// push constant (FS FAU bytes 0..15): colour
layout(location=0) out vec4 out_color;
layout(push_constant) uniform P { vec4 color; } pc;
void main(){ out_color = pc.color; }
