#version 450
// textured quad FS: sampler2D = combined image sampler of user set 0 binding 0 (FS resource table entry 1, descriptors 0 = texture, 1 = sampler).
// The vertex shader is pos.vert: its `col` output carries the texture coordinate in .xy.
layout(location=0) in vec4 v_col;
layout(location=0) out vec4 out_color;
layout(set=0, binding=0) uniform sampler2D tex;
void main(){ out_color = texture(tex, v_col.xy); }
