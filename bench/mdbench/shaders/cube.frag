#version 450
layout(set=0,binding=0) uniform U { mat4 mvp; mat4 model; vec4 color; vec4 light; } u;
layout(set=0,binding=1) uniform sampler2D tex0;
layout(location=0) in vec3 nrm;
layout(location=1) in vec2 uv;
layout(location=2) in vec3 col;
layout(location=0) out vec4 o;
void main(){
  float d = max(dot(normalize(nrm), normalize(u.light.xyz)), 0.0);
  vec3 t = texture(tex0, uv * 2.0).rgb;
  o = vec4(t * col * (0.2 + 0.8*d), 1.0);
}
