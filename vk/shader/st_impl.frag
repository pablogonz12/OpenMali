#version 450
// RUNG2 state sweep: implicit-LOD sample (derivatives -> minFilter, mipmapMode, anisotropy).
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_color;
layout(set=0, binding=0) uniform sampler2D tex;
layout(push_constant) uniform P { vec4 uvs; vec4 col; vec4 posz; } pc;
void main(){ out_color = texture(tex, v_uv); }
