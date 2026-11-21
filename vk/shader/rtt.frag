#version 450
// t_vk_xcb --rtt draw: the fragment colour the offscreen image is drawn with (0.1,0.45,0.9 = RGB 26,115,230),
// so a --rtt draw and a --rtt clear must produce the same pixel when the sampling draw reads the image back.
layout(location=0) out vec4 o;
void main(){ o = vec4(0.1, 0.45, 0.9, 1.0); }
