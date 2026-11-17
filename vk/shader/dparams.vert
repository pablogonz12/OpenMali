#version 460
// R4-B2 shaderDrawParameters: a right triangle in column <col>/row <row> only if every draw parameter has its expected value (else off screen).
//  A (BaseInstance 1..): indirect draws, BaseVertex = 4+DrawIndex, BaseInstance = 1+2*DrawIndex, column DrawIndex
//  B (BaseInstance 100): direct draw, BaseVertex 9, DrawIndex 0, column 3
//  C (BaseInstance 200): direct indexed draw, vertexOffset 5, DrawIndex 0, column 0, row 1
const vec2 p[3] = vec2[3](vec2(-.25,-.25), vec2(.25,-.25), vec2(-.25,.25));
void main(){
  int vi = clamp(gl_VertexIndex - gl_BaseVertex, 0, 2);
  bool ok; int col; float row = 0.0;
  if (gl_BaseInstance < 100) { ok = gl_BaseVertex == 4 + gl_DrawID && gl_BaseInstance == 1 + 2 * gl_DrawID; col = gl_DrawID; }
  else if (gl_BaseInstance < 150) { ok = gl_BaseVertex == 9 && gl_BaseInstance == 100 && gl_DrawID == 0; col = 3; }
  else { ok = gl_BaseVertex == 5 && gl_BaseInstance == 200 && gl_DrawID == 0; col = 0; row = 0.5; }
  ok = ok && gl_InstanceIndex == gl_BaseInstance && gl_VertexIndex - gl_BaseVertex == vi;
  vec2 q = ok ? p[vi] + vec2(-0.75 + 0.5 * float(col), row) : vec2(5.0, 5.0);
  gl_Position = vec4(q, 0.5, 1.0);
}
