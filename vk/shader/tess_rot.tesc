#version 450
// T2's device reference for bar.tesc: the same rotation of control-point colours, written without a barrier (every
// invocation reads only the patch *inputs* of another invocation, which a TCS may do). bar.tesc reaches the same picture
// only if its cross-invocation write-then-read through the tessellation blob is ordered by poly's barrier, so the two
// frames must be identical pixel for pixel.
layout(vertices = 3) out;
layout(location = 0) in vec4 v_col[];
layout(location = 0) out vec4 tc_col[];
void main(){
  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
  tc_col[gl_InvocationID] = v_col[(gl_InvocationID + 1) % 3];
  gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0; gl_TessLevelOuter[3] = 1.0;
  gl_TessLevelInner[0] = 1.0; gl_TessLevelInner[1] = 1.0;
}
