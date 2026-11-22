#version 450
// multi-render-target test: no vertex inputs; two triangles covering the left half of the target (x in [-1,0])
void main() {
   vec2 p[6] = vec2[](vec2(-1,-1), vec2(0,-1), vec2(0,1), vec2(-1,-1), vec2(0,1), vec2(-1,1));
   gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
