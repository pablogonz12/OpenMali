#version 450
layout(location = 0) flat in uint ov;
layout(location = 0) out uint col;
void main() { col = ov; }
