#version 450
// t_vk_xcb --blend: red at 50% alpha, blended over the blue clear (srcAlpha / 1-srcAlpha).
layout(location = 0) out vec4 o;
void main(){ o = vec4(1.0, 0.0, 0.0, 0.5); }
