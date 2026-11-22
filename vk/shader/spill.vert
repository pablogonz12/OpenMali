#version 450
// Graphics thread-local storage: a const array indexed by gl_VertexIndex does not fit in registers, so the compiler
// spills it to scratch (tls_size 144 on Mali). Three full-height thirds (18 vertices) cover the whole target, so every
// pixel must be solid. A draw without TLS stores to address 0 and the device is lost.
const vec2 p[18] = vec2[18](
   vec2(0.0,     0.0), vec2(0.0,     1.0), vec2(1.0/3.0, 0.0),
   vec2(0.0,     1.0), vec2(1.0/3.0, 1.0), vec2(1.0/3.0, 0.0),
   vec2(1.0/3.0, 0.0), vec2(1.0/3.0, 1.0), vec2(2.0/3.0, 0.0),
   vec2(1.0/3.0, 1.0), vec2(2.0/3.0, 1.0), vec2(2.0/3.0, 0.0),
   vec2(2.0/3.0, 0.0), vec2(2.0/3.0, 1.0), vec2(1.0,     0.0),
   vec2(2.0/3.0, 1.0), vec2(1.0,     1.0), vec2(1.0,     0.0));
void main(){
  gl_Position = vec4(p[gl_VertexIndex] * 2.0 - 1.0, 0.5, 1.0);
}
