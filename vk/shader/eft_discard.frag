#version 450
// EarlyFragmentTests + an application discard: early tests write depth before the discard, late tests would not.
// With cull distances (which force late tests) the pipeline must be refused.
layout(early_fragment_tests) in;
layout(location = 0) out vec4 o;
void main(){ if (gl_FragCoord.x < 8.0) discard; o = vec4(1.0, 0.0, 0.0, 1.0); }
