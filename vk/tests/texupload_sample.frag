#version 450
// 1:1 sample (normalised coordinates, nearest) of the texture into the RGBA8 target.
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { o = texture(t, gl_FragCoord.xy / vec2(textureSize(t, 0))); }
