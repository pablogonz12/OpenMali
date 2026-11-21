#version 450
/* 256-byte push block: reads the last word (offset 252) and uses constants often enough that the compiler promotes them to FAU
 * immediates. The immediates must land ABOVE the push block (fau_reserved covers it); with the old default reservation the
 * emitter overwrote push constants with immediates (or vice versa) for blocks over 56 B in the VS. */
layout(push_constant) uniform PC { vec4 a[16]; } pc;
void main()
{
   float w = pc.a[15].w;            /* offset 252 */
   float s = w;
   /* each constant used 6x: well above the promotion threshold, and not foldable (s changes every step) */
   s = s * 1.5 + 0.375;
   s = s * 1.5 + 0.375;
   s = s * 1.5 + 0.375;
   s = s * 0.25 + 0.125;
   s = s * 0.25 + 0.125;
   s = s * 0.25 + 0.125;
   s = s * 2.0 + 0.5;
   s = s * 2.0 + 0.5;
   s = s * 2.0 + 0.5;
   /* full-screen triangle from the vertex index (no vertex buffers): the push value reaches the FS as the colour */
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
   gl_Position.z = (s - w) * 1e-20;   /* keep s live without moving the triangle (not foldable) */
}
