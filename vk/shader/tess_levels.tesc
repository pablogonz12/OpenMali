#version 450
// tess_reject: the identity TCS with level 2.0 — the patch would be subdivided, so the vertex stream the VS wrote is not
// the one the tessellator would produce.
layout(vertices = 3) out;
layout(location=0) in vec4 v_col[];
layout(location=0) out vec4 tc_col[];
void main(){
  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
  tc_col[gl_InvocationID] = v_col[gl_InvocationID];
  gl_TessLevelOuter[0] = 2.0; gl_TessLevelOuter[1] = 2.0; gl_TessLevelOuter[2] = 2.0; gl_TessLevelOuter[3] = 2.0;
  gl_TessLevelInner[0] = 2.0; gl_TessLevelInner[1] = 2.0;
}
