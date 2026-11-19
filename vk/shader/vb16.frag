#version 450
layout(location = 0) in vec2 v;
layout(location = 0) out vec4 o;
void main()
{
   o = vec4(v.x, 0.0, 0.0, 1.0);
}
