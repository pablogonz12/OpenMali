#version 450
// R4 colour loadOp LOAD: full-viewport triangle (3 vertices, no vertex buffers)
void main() {
   vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
