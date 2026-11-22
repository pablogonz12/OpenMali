#version 450
layout(location=0) in flat int k;
layout(location=0) out vec4 o;
void main(){ o = k == 0 ? vec4(1,0,0,1) : k == 1 ? vec4(0,1,0,1) : k == 2 ? vec4(0,0,1,1) : vec4(1,1,0,1); }
