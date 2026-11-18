#version 450
// specialization constants (t_vk_feat spec, t_compile spec): colour from three constants with non-adjacent ids
layout(constant_id = 0) const float R = 0.25;
layout(constant_id = 3) const uint MODE = 0;
layout(constant_id = 5) const bool BLUE = false;
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(R, MODE == 2u ? 1.0 : 0.0, BLUE ? 1.0 : 0.0, 1.0); }
