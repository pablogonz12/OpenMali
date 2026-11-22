#version 450
/* One row of derivative facts per draw (pc.lod = row), written to an RGBA32F target:
 * 0 dFdx(gl_FragCoord.x), dFdy(gl_FragCoord.y)
 * 1 dFdx(smooth varying), dFdy(smooth varying)
 * 2 dFdx(flat varying)
 * 3 dFdx(value computed only from gl_FragCoord)
 * 4 dFdxFine(varying), dFdxCoarse(varying)
 * 5 dFdxFine(gl_FragCoord.x), dFdxCoarse(gl_FragCoord.x)
 * 6 gl_FragCoord.xy, varying, flat value (raw lane values)
 * 7 dFdy(gl_FragCoord.x), dFdx(gl_FragCoord.y) */
layout(push_constant) uniform PC { float lod; float scale; } pc;
layout(location = 0) in vec2 vUV;
layout(location = 1) flat in float flatv;
layout(location = 0) out vec4 o;
void main()
{
   float u = vUV.x;
   float fromfc = gl_FragCoord.x / 64.0;
   int row = int(pc.lod + 0.5);
   vec4 v;
   if (row == 0)      v = vec4(dFdx(gl_FragCoord.x), dFdy(gl_FragCoord.y), 0.0, 0.0);
   else if (row == 1) v = vec4(dFdx(u), dFdy(u), 0.0, 0.0);
   else if (row == 2) v = vec4(dFdx(flatv), 0.0, 0.0, 0.0);
   else if (row == 3) v = vec4(dFdx(fromfc), 0.0, 0.0, 0.0);
   else if (row == 4) v = vec4(dFdxFine(u), dFdxCoarse(u), 0.0, 0.0);
   else if (row == 5) v = vec4(dFdxFine(gl_FragCoord.x), dFdxCoarse(gl_FragCoord.x), 0.0, 0.0);
   else if (row == 6) v = vec4(gl_FragCoord.x, gl_FragCoord.y, u, flatv);
   else               v = vec4(dFdy(gl_FragCoord.x), dFdx(gl_FragCoord.y), 0.0, 0.0);
   o = v;
}
