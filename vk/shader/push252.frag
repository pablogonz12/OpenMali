#version 450
/* 256-byte push block: the colour is the word at offset 252 (last 4 bytes). Constants repeated 6x force FAU immediates, which must
 * not share a word with the push block (FS push block starts at FAU word 0 on v10/v11, word 6 on v13: blend words 4,5). */
layout(push_constant) uniform PC { vec4 a[16]; } pc;
layout(location = 0) out vec4 o;
void main()
{
   float w = pc.a[15].w;            /* offset 252 */
   float s = w;
   s = s * 1.5 + 0.375;
   s = s * 1.5 + 0.375;
   s = s * 1.5 + 0.375;
   s = s * 0.25 + 0.125;
   s = s * 0.25 + 0.125;
   s = s * 0.25 + 0.125;
   s = s * 2.0 + 0.5;
   s = s * 2.0 + 0.5;
   s = s * 2.0 + 0.5;
   o = vec4(w, (s - w) * 1e-20, 0.0, 1.0);   /* keeps the chain live; green stays 0 in 8 bits */
}
