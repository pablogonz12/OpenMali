#version 450
void main(){
  // line list: one horizontal line at y=0 from x=-0.5 to 0.5
  gl_Position = vec4(gl_VertexIndex == 0 ? -0.5 : 0.5, 0.0, 0.0, 1.0);
}
