#version 450
// Vulkan: gl_FragCoord.xy is a pixel centre (x + 0.5, y + 0.5). Mali hardware reports the integer corner; nir_lower_wpos_center adds the 0.5.
layout(location=0) out vec4 o;
void main(){ o = vec4(gl_FragCoord.x - 100.0, gl_FragCoord.y - 200.0, 0.0, 1.0); }
