#version 450
/* Format sweep, vertex half (notes/format-sweep.md): a screen quad built from gl_VertexIndex, whose only data are the
 * tested attribute. One pipeline per (format, width): the width variant declares the attribute as vec4 or as vec2, so
 * the number of components the driver is asked to fetch is exactly the shader's own type (no duplicate locations: two
 * declarations of the same bytes at different locations made the driver fetch one width for both, see the notes).
 * -DVT=uvec4/ivec4 (and -DVT2=uvec2/ivec2) select the integer classes. */
#ifndef VT
#define VT vec4
#endif
#ifndef VT2
#define VT2 vec2
#endif
#ifdef VEC2
layout(location = 0) in VT2 a0;   /* binding 0, attribute offset 4 */
layout(location = 1) in VT2 a1;   /* binding 1, attribute offset 8: a second stream */
#else
layout(location = 0) in VT a0;
layout(location = 1) in VT a1;
#endif
layout(location = 0) flat out vec4 v;
layout(push_constant) uniform PC { vec4 scale; vec4 bias; ivec4 c; } pc;   /* c.x = which attribute to pass on */
void main() {
   int i = gl_VertexIndex;
   vec2 p = vec2((i == 1 || i == 2 || i == 4) ? 1.0 : -1.0,
                 (i == 2 || i == 4 || i == 5) ? 1.0 : -1.0);
   gl_Position = vec4(p, 0.0, 1.0);
#ifdef VEC2
   v = pc.c.x == 0 ? vec4(float(a0.x), float(a0.y), 0.0, 1.0) : vec4(float(a1.x), float(a1.y), 0.0, 1.0);
#else
   v = pc.c.x == 0 ? vec4(float(a0.x), float(a0.y), float(a0.z), float(a0.w))
                   : vec4(float(a1.x), float(a1.y), float(a1.z), float(a1.w));
#endif
}
