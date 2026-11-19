#version 450
/* Reference scene vertex stage (vk/tests/ref_scene.h): 2D positions in NDC, per-draw z from the push constant. */
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;

layout(push_constant) uniform PC {
   float z;
   float alpha;
   float tex;
   float pad;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_col;

void main()
{
   gl_Position = vec4(a_pos, pc.z, 1.0);
   v_uv = a_uv;
   v_col = vec4(a_col.rgb, a_col.a * pc.alpha);
}
