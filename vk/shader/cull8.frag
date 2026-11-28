#version 450
// Reads all 16 vec4 varyings of cull8.vert, so the FS input budget is full when the cull flags are appended.
layout(location = 0) in vec4 v[16];
layout(location = 0) out vec4 o;
void main(){ vec4 s = vec4(0.0); for (int i = 0; i < 16; i++) s += v[i]; o = vec4(s.x > 0.0 ? 1.0 : 0.0, 0.0, 0.0, 1.0); }
