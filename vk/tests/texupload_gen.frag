#version 450
// a.y == 0: pattern, channel c = (x*7 + y*13 + c*61 + a.x*29) & a.z; else the constant a.x in every channel (blend pass).
layout(push_constant) uniform P { ivec4 a; vec4 rect; } p;
layout(location = 0) out vec4 o;
void main() {
   ivec2 f = ivec2(gl_FragCoord.xy);
   if (p.a.y != 0) { o = vec4(float(p.a.x) / 255.0); return; }
   ivec4 v = (ivec4(f.x * 7 + f.y * 13 + p.a.x * 29) + ivec4(0, 61, 122, 183)) & p.a.z;
   o = vec4(v) / 255.0;
}
