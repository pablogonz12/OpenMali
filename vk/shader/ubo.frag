#version 450
layout(location=0) in vec4 v_col;
layout(location=0) out vec4 out_color;
layout(set=0, binding=0) uniform U { vec4 colour; } u;
void main(){ out_color = u.colour; }
