#version 450
layout(location=0) in vec4 v_col;
layout(location=0) out vec4 out_color;
layout(push_constant) uniform P { vec4 rot; vec4 colour; } pc;
void main(){ out_color = pc.colour; }
