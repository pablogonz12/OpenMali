#version 450
/* Format sweep, texture half: the screen quad for the sampled-format rows (no vertex data at all). */
void main() {
   int i = gl_VertexIndex;
   vec2 p = vec2((i == 1 || i == 2 || i == 4) ? 1.0 : -1.0,
                 (i == 2 || i == 4 || i == 5) ? 1.0 : -1.0);
   gl_Position = vec4(p, 0.0, 1.0);
}
