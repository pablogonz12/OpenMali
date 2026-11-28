#version 450
// A quad given in NDC by push constants; st = unnormalised position inside it (used by deriv).
layout(push_constant) uniform P { ivec4 a; vec4 rect; } p;
layout(location = 0) out vec2 st;
layout(location = 1) flat out int idx;
void main() {
   vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
   st = c * vec2(p.a.yz);
   idx = p.a.x;
   gl_Position = vec4(mix(p.rect.xy, p.rect.zw, c), 0.0, 1.0);
}
