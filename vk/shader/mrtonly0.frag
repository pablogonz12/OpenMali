#version 450
layout(location=0) out vec4 o0;
void main(){ o0 = vec4(gl_FragCoord.x/256.0, 0.0, 0.0, 1.0); }
