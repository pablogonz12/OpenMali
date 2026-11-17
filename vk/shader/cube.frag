#version 450
layout(location=0) in vec4 v_col;
layout(location=0) out vec4 out_color;
layout(set=0, binding=0) uniform sampler2D tex;
void main(){ vec4 t = texture(tex, v_col.xy); out_color = vec4(t.rgb * v_col.z, 1.0); }
