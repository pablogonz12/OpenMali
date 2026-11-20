#version 450
/* Solid colour: the CPU benchmark must not be limited by fill or ALU. */
layout(location = 0) out vec4 o;

void main()
{
   o = vec4(0.25, 0.5, 0.75, 1.0);
}
