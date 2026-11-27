#version 450
// integer vertex attribute -> flat int varying -> FS (the CTS descriptorIndexing_vert* pattern)
layout(location=0) in ivec4 idx;
layout(location=0) flat out int k;
const vec2 p[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1));
void main(){ vec2 q = p[gl_VertexIndex]; gl_Position = vec4(q * 2.0 - 1.0, 0.5, 1.0); k = idx.x; }
