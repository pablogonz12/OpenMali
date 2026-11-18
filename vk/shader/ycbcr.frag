#version 450
// samplerYcbcrConversion (t_vk_feat ycbcr): one immutable-sampler NV12 combined image sampler; the driver lowers the conversion.
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 out_color;
layout(set=0, binding=0) uniform sampler2D tex;
void main(){ out_color = texture(tex, v_uv); }
