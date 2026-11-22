#version 450
// notes/gs-design.md §6: the "rast" vertex shader of the compute-pre-pass design. No vertex buffers are bound: every
// attribute comes from the storage buffer a compute job filled (set 0, binding 1), indexed by the vertex index the draw
// supplies. Same clip positions and colours as rot.vert + the triangle's vertex buffers, so the picture must match.
struct V { vec4 clip; vec4 col; };
layout(set = 0, binding = 1) readonly buffer Dst { V v[]; } dst;
layout(location = 0) out vec4 v_col;
void main() {
   V x = dst.v[gl_VertexIndex];
   gl_Position = x.clip;
   v_col = x.col;
}
