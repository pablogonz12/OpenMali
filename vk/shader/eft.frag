#version 450
// EarlyFragmentTests, nothing late tests could change: with cull distances the pipeline is accepted (late Z/S is equivalent).
layout(early_fragment_tests) in;
layout(location = 0) out vec4 o;
void main(){ o = vec4(1.0, 0.0, 0.0, 1.0); }
