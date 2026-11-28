#version 450
// t_skia_draws: Skia-shaped draw (pos + uv attributes from a dynamic vertex buffer, per-draw push-constant transform).
layout(location=0) in vec2 pos;
layout(location=1) in vec2 uv;
layout(location=0) out vec2 v_uv;
layout(push_constant) uniform P { vec4 xf; } pc;
void main(){ gl_Position = vec4(pos * pc.xf.xy + pc.xf.zw, 0.0, 1.0); v_uv = uv; }
