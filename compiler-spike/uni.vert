#version 450
// push constants (VS FAU bytes 32..): scale.xy, offset.zw, z.  Viewport sysvals stay at FAU bytes 0..23.
layout(location=0) in vec4 pos;
layout(push_constant) uniform P { vec4 so; vec4 zz; } pc;
void main(){ gl_Position = vec4(pos.xy * pc.so.xy + pc.so.zw, pc.zz.x, 1.0); }
