#version 450
// position + index from an integer vertex attribute (the CTS layout: ivec4 at location 2, 16 B stride)
layout(location=0) in vec4 in_position;
layout(location=1) in ivec4 index;
layout(location=0) flat out int k;
void main(){
  gl_Position = in_position;
  gl_PointSize = 1.0;
  k = index.x;
}
