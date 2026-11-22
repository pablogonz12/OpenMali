#version 450
// tess_reject: moves control point 0 — the TCS is not the identity patch, so the stream differs.
layout(vertices = 3) out;
layout(location=0) in vec4 v_col[];
layout(location=0) out vec4 tc_col[];
void main(){
  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position + vec4(0.1, 0.0, 0.0, 0.0);
  tc_col[gl_InvocationID] = v_col[gl_InvocationID];
  gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0; gl_TessLevelOuter[3] = 1.0;
  gl_TessLevelInner[0] = 1.0; gl_TessLevelInner[1] = 1.0;
}
