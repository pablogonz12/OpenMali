#version 450
layout(push_constant) uniform P { vec4 p0; vec4 p1; } pc;   // p0.z = iterations
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){
  int n = int(pc.p0.z);
  vec4 v = vec4(uv, uv.x*uv.y, 1.0);
  for (int i = 0; i < n; i++) {
    v = fract(v * 1.0001 + sin(v.yzwx) * 0.37) + 0.01 * v.wxyz;
  }
  o = vec4(v.xyz, pc.p0.y);
}
