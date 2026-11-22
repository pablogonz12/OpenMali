#version 450
layout(location=2) out vec4 o2;
void main(){ o2 = vec4(gl_FragCoord.x/256.0, 0.0, 0.0, 1.0); }
