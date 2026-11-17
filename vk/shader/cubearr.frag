#version 450
// R4-B2: four quadrants sample (cube 0, +X), (cube 0, -Y), (cube 1, +X), (cube 1, +Z) of a samplerCubeArray.
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform samplerCubeArray tex;
void main(){
  int q = (v_uv.x >= 0.5 ? 1 : 0) + (v_uv.y >= 0.5 ? 2 : 0);
  vec3 d = q == 0 ? vec3(1,0,0) : q == 1 ? vec3(0,-1,0) : q == 2 ? vec3(1,0,0) : vec3(0,0,1);
  o = texture(tex, vec4(d, q >= 2 ? 1.0 : 0.0));
}
