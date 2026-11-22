#version 450
// mixed-format MRT test: RGBA8, RGBA16F (additive blend), R32_UINT and a write-masked RGBA8
layout(location=0) out vec4 o0;
layout(location=1) out vec4 o1;
layout(location=2) out uint o2;
layout(location=3) out vec4 o3;
void main(){ o0 = vec4(0.5, 0.25, 1.0, 1.0); o1 = vec4(0.5, 0.0, 0.0, 1.0); o2 = 1234u; o3 = vec4(1.0, 1.0, 0.0, 0.0);   /* every channel differs from RT3's (0,0,1,1) clear: a wrongly enabled write shows */ }
