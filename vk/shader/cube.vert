#version 450
// M14 t_vk_present: rotating cube. Push constant: rot = (cosY, sinY, cosX, sinX), proj = (f/aspect, f, 0, 0).
// col attribute = (u, v, shade, 1); passed through as one vec4 varying (as var.frag does).
layout(location=0) in vec4 pos;
layout(location=1) in vec4 col;
layout(location=0) out vec4 v_col;
layout(push_constant) uniform P { vec4 rot; vec4 proj; } pc;
void main(){
  vec3 p = pos.xyz;
  vec3 q = vec3(p.x * pc.rot.x + p.z * pc.rot.y, p.y, -p.x * pc.rot.y + p.z * pc.rot.x);
  vec3 r = vec3(q.x, q.y * pc.rot.z - q.z * pc.rot.w, q.y * pc.rot.w + q.z * pc.rot.z);
  r.z += 3.0;
  gl_Position = vec4(r.x * pc.proj.x, r.y * pc.proj.y, 1.25 * r.z - 1.25, r.z);   // near 1, far 5
  v_col = col;
}
