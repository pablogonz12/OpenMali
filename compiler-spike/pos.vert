#version 450
layout(location=0) in vec4 pos;
layout(location=1) in vec4 col;
layout(location=0) out vec4 v_col;
void main(){ gl_Position = pos; v_col = col; }
