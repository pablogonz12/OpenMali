#version 450
// sampleRateShading at 1 sample: gl_SampleID must read 0 and gl_SamplePosition the pixel centre; red = ok (blue = absolute position error, so a wrong position is not clamped away by UNORM).
layout(location=0) sample in float v;   // per-sample interpolation qualifier
layout(location=0) out vec4 o;
void main(){ o = vec4(1.0, float(gl_SampleID) + v * 0.0, abs(gl_SamplePosition.x - 0.5) + abs(gl_SamplePosition.y - 0.5), 1.0); }
