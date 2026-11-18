#version 450
// first_instance / vertex-buffer-rebind reuse tests (t_vk_draw share_inst): one per-instance vec4 attribute (xy = offset) moves a small right triangle.
layout(location=0) in vec4 off;
layout(location=0) out vec4 v_col;
const vec2 p[3] = vec2[3](vec2(-.25,-.25), vec2(.25,-.25), vec2(-.25,.25));
void main(){
  gl_Position = vec4(p[gl_VertexIndex] + off.xy, 0.5, 1.0);
  v_col = vec4(1.0);
}
