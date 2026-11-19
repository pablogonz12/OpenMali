#version 450
/* Reference scene fragment stage. The texture branch is uniform per draw, so pc.tex == 0 gives exactly the vertex
 * colour and pc.tex == 1 exactly the texture: no arithmetic that could differ between RADV and Mali. */
layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;

layout(push_constant) uniform PC {
   float z;
   float alpha;
   float tex;
   float pad;
} pc;

layout(location = 0) out vec4 o_col;

void main()
{
   vec3 c = v_col.rgb;
   if (pc.tex > 0.5)
      c *= texture(u_tex, v_uv).rgb;
   o_col = vec4(c, v_col.a);
}
