#version 450
layout(push_constant) uniform P { vec4 p0; vec4 p1; } pc;   // p0.x = layer index, p0.y = alpha
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){
  float l = pc.p0.x;
  vec3 c = 0.5 + 0.5*cos(vec3(0.0,2.1,4.2) + l*1.7 + uv.x*3.0 + uv.y*2.0);
  o = vec4(c, pc.p0.y);
}
