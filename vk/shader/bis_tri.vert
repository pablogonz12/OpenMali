#version 450
/* The interp-sweep triangle shape: NDC (-0.9,-0.9) (0.9,-0.9) (0,0.9), 3 varyings. */
layout(location = 0) out vec3 v0;
layout(location = 1) out vec3 v1;
layout(location = 2) out vec3 v2;
void main()
{
   const vec2 c[3] = vec2[3](vec2(-0.9, -0.9), vec2(0.9, -0.9), vec2(0.0, 0.9));
   vec2 p = c[gl_VertexIndex];
   vec2 uv = (p + 1.0) * 0.5;
   v0 = vec3(uv, 0.25);
   v1 = vec3(uv, 0.5);
   v2 = vec3(uv, 0.75);
   gl_Position = vec4(p, 0.0, 1.0);
}
