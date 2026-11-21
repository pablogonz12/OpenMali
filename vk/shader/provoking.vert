#version 450
// VK_EXT_provoking_vertex: one covering triangle, flat colour per vertex (v0 red, v1 green, v2 blue).
layout(location=0) flat out vec4 c;
const vec2 p[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
const vec4 col[3] = vec4[3](vec4(1,0,0,1), vec4(0,1,0,1), vec4(0,0,1,1));
void main(){ gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0); c = col[gl_VertexIndex]; }
