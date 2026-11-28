#version 450
// EarlyFragmentTests + a gl_FragDepth write: EFT ignores the written depth, late tests would use it -> refused with cull.
layout(early_fragment_tests) in;
layout(location = 0) out vec4 o;
void main(){ gl_FragDepth = 0.25; o = vec4(1.0, 0.0, 0.0, 1.0); }
