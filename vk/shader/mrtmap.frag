#version 450
// permutation probe: output i carries (i+1)/10 * (gl_FragCoord.x / 256), so each target's pixel names its
// source output even if constant stores are coalesced by the register allocator
layout(location=0) out vec4 o0;
layout(location=1) out vec4 o1;
layout(location=2) out vec4 o2;
layout(location=3) out vec4 o3;
void main(){
   float f = gl_FragCoord.x / 256.0;
   o0 = vec4(0.1 * f); o1 = vec4(0.2 * f); o2 = vec4(0.3 * f); o3 = vec4(0.4 * f);
}
