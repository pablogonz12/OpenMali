#version 450
layout(set=0,binding=2) uniform sampler2DArray texa;
layout(push_constant) uniform P { vec4 p0; vec4 p1; } pc;   // p0.w = samples, p1.x = layers, p1.y = uv scale
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){
  int n = int(pc.p0.w);
  int layers = int(pc.p1.x);
  vec4 acc = vec4(0.0);
  for (int i = 0; i < n; i++) {
    float fi = float(i);
    vec2 c = uv * pc.p1.y * (1.0 + 0.13 * fi) + vec2(0.07 * fi, 0.031 * fi);
    acc += texture(texa, vec3(c, float(i % layers)));
  }
  o = vec4(acc.rgb / float(max(n,1)), pc.p1.z);
}
