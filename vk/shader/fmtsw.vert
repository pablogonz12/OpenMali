#version 450
/* Format sweep, vertex half (notes/format-sweep.md): a screen quad built from gl_VertexIndex, whose only data are the
 * two tested attributes. Each attribute is declared twice, as vec4 and as vec2, so ONE pipeline per format covers both
 * component counts (the vec2 locations read the same bytes, only two components are consumed).
 * The tested format is baked into the pipeline's vertex-input state; the shader sees whatever the driver supplies.
 * -DVT=uvec4/ivec4 selects the integer classes (a uvec/ivec input is the only legal declaration for those formats). */
#ifndef VT
#define VT vec4
#endif
#ifndef V2T
#define V2T vec2
#endif
layout(location = 0) in VT a0;      /* binding 0, attribute offset 4 */
layout(location = 1) in VT a1;      /* binding 1, attribute offset 8 (a second stream) */
layout(location = 2) in V2T a0v2;   /* the same bytes as a0, two components consumed */
layout(location = 3) in V2T a1v2;
layout(location = 0) flat out vec4 v;
layout(push_constant) uniform PC { vec4 scale; vec4 bias; ivec4 c; } pc;   /* c.x = which attribute to pass on */
vec4 to4(VT a) { return vec4(float(a.x), float(a.y), float(a.z), float(a.w)); }
vec4 to2(V2T a) { return vec4(float(a.x), float(a.y), 0.0, 1.0); }
void main() {
   int i = gl_VertexIndex;
   vec2 p = vec2((i == 1 || i == 2 || i == 4) ? 1.0 : -1.0,
                 (i == 2 || i == 4 || i == 5) ? 1.0 : -1.0);
   gl_Position = vec4(p, 0.0, 1.0);
   int w = pc.c.x;
   v = w == 0 ? to4(a0) : w == 1 ? to4(a1) : w == 2 ? to2(a0v2) : to2(a1v2);
}
