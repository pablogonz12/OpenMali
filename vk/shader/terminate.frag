#version 450
#extension GL_EXT_terminate_invocation : require
// shaderTerminateInvocation: even-x pixels terminate (no colour write, alpha stays 0); odd-x pixels write opaque green.
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){
  if ((int(gl_FragCoord.x) & 1) == 0) terminateInvocation;
  o = vec4(0.0, 1.0, 0.0, 1.0);
}
