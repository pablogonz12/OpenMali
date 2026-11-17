#version 450
#extension GL_EXT_demote_to_helper_invocation : require
// R4-B2 shaderDemoteToHelperInvocation: even-x pixels demote, odd-x pixels must still get a valid dFdx (the demoted neighbour stays a helper).
layout(location=0) in vec2 uv;      // uv = pixel coordinates (push constant scale 256)
layout(location=0) out vec4 o;
void main(){
  if ((int(gl_FragCoord.x) & 1) == 0) demote;
  float d = dFdx(uv.x);             // 1.0 per pixel
  o = vec4(d * 0.5, 1.0, helperInvocationEXT() ? 1.0 : 0.0, 1.0);
}
