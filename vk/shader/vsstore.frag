#version 450
// vertexPipelineStoresAndAtomics: the fragment stage only passes the vertex colour through, so anything the SSBO gets came
// from the vertex stage (its binding is VK_SHADER_STAGE_VERTEX_BIT only).
layout(location=0) in vec4 c;
layout(location=0) out vec4 o;
void main(){ o = c; }
