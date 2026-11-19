#version 450
/* One vec2 attribute per vertex binding 0..15 (maxVertexInputAttributes/maxVertexInputBindings = 16). The sum of the 16 x
 * components goes to the colour, so a binding/attribute that never reaches the shader shows up as a wrong value. */
layout(location = 0) in vec2 a0;
layout(location = 1) in vec2 a1;
layout(location = 2) in vec2 a2;
layout(location = 3) in vec2 a3;
layout(location = 4) in vec2 a4;
layout(location = 5) in vec2 a5;
layout(location = 6) in vec2 a6;
layout(location = 7) in vec2 a7;
layout(location = 8) in vec2 a8;
layout(location = 9) in vec2 a9;
layout(location = 10) in vec2 a10;
layout(location = 11) in vec2 a11;
layout(location = 12) in vec2 a12;
layout(location = 13) in vec2 a13;
layout(location = 14) in vec2 a14;
layout(location = 15) in vec2 a15;
layout(location = 0) out vec2 v;
void main()
{
   float s = a0.x + a1.x + a2.x + a3.x + a4.x + a5.x + a6.x + a7.x +
             a8.x + a9.x + a10.x + a11.x + a12.x + a13.x + a14.x + a15.x;
   v = vec2(s, 0.0);
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
