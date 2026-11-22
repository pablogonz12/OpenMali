#version 450
// shaderCullDistance, declared but not (fully) written: gl_CullDistance[2] is declared and only [0] is stored (+1). The unwritten
// [1] must not cull anything: the whole target is solid. A lowering that discards on a flag nobody stores culls every fragment.
out gl_PerVertex { vec4 gl_Position; float gl_CullDistance[2]; };
const vec2 p[6] = vec2[6](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
void main(){
  gl_Position = vec4(p[gl_VertexIndex] * 2.0 - 1.0, 0.5, 1.0);
  gl_CullDistance[0] = 1.0;
}
