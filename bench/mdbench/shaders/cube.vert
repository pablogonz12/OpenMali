#version 450
layout(location=0) in vec3 in_pos;
layout(location=1) in vec3 in_nrm;
layout(location=2) in vec2 in_uv;
layout(set=0,binding=0) uniform U { mat4 mvp; mat4 model; vec4 color; vec4 light; } u;
layout(location=0) out vec3 nrm;
layout(location=1) out vec2 uv;
layout(location=2) out vec3 col;
void main(){
  gl_Position = u.mvp * vec4(in_pos,1.0);
  nrm = mat3(u.model) * in_nrm;
  uv = in_uv; col = u.color.rgb;
}
