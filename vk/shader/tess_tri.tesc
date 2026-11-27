#version 450
// T1's identity TCS: three control points, every per-vertex output copied from the same control point, and the six tess
// level slots the constant 1.0. In Vulkan the levels are TCS outputs, so this stage is what a level-1 tess pipeline needs
// (a vertex shader cannot write gl_TessLevel*). At level 1 a triangle patch tessellates to exactly its three control
// points, so VS + this + the pass-through TES produce the stream the VS alone produces.
layout(vertices = 3) out;
layout(location=0) in vec4 v_col[];
layout(location=0) out vec4 tc_col[];
void main(){
  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
  tc_col[gl_InvocationID] = v_col[gl_InvocationID];
  gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0; gl_TessLevelOuter[3] = 1.0;
  gl_TessLevelInner[0] = 1.0; gl_TessLevelInner[1] = 1.0;
}
