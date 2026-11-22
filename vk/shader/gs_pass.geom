#version 450
// Lane 1 (notes/gs-design.md §4): a *provable* pass-through geometry shader -- 1-in/1-out, in order, one stream, every
// output a copy of the matching input vertex. t_vk_draw --only gs_passthrough draws this and the same pipeline without
// the geometry stage and requires the identical picture (diff 0 px) and no extra work in the frame.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 in_col[];
layout(location = 0) out vec4 v_col;
void main(){
  gl_Position = gl_in[0].gl_Position; v_col = in_col[0]; EmitVertex();
  gl_Position = gl_in[1].gl_Position; v_col = in_col[1]; EmitVertex();
  gl_Position = gl_in[2].gl_Position; v_col = in_col[2]; EmitVertex();
  EndPrimitive();
}
