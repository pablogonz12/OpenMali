#version 450
/* Two triangles covering exactly the target, 3 varyings. */
layout(location = 0) out vec3 v0;
layout(location = 1) out vec3 v1;
layout(location = 2) out vec3 v2;
void main()
{
   const vec2 c[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(-1, 1), vec2(1, -1), vec2(1, 1));
   vec2 p = c[gl_VertexIndex];
   vec2 uv = (p + 1.0) * 0.5;
   v0 = vec3(uv, 0.25);
   v1 = vec3(uv, 0.5);
   v2 = vec3(uv, 0.75);
   gl_Position = vec4(p, 0.0, 1.0);
}
