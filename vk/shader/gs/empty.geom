#version 450
/* triangles in, nothing emitted: the padded index slot stays all-restart */
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec3 in_col[];
layout(location = 0) out vec3 out_col;
void main() {
}
