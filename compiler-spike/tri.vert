#version 450
layout(location=0) in vec4 pos;
layout(location=0) out vec4 v_col;
void main(){ gl_Position = pos; v_col = vec4(1.0, 0.5, 0.25, 1.0); }
