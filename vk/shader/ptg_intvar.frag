#version 450
layout(location=0) in flat int k;
layout(location=0) out vec4 o;
void main(){ o = vec4(k == 0 ? 1.0 : 0.5, 0.5, 0.5, 1.0); }
