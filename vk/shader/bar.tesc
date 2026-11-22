#version 450
// T2's falsifier for the one primitive the compute form has to preserve: barrier(). Invocation i writes its own control
// point's colour, waits, then takes the colour of control point i+1 as its position — so on the device every output is the
// *rotated* neighbour's colour. Without the barrier, or with the TCS's three invocations writing to one address (the
// stride-24 bug), the reads return garbage or the invocation's own write. Compiled by glslang:
//   glslangValidator -V --target-env vulkan1.1 bar.tesc -o bar.tesc.spv
layout(vertices = 3) out;
layout(location = 0) in vec4 v_col[];
layout(location = 0) out vec4 tc_col[];
void main(){
  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
  tc_col[gl_InvocationID] = v_col[gl_InvocationID];
  barrier();
  vec4 n = tc_col[(gl_InvocationID + 1) % 3];
  tc_col[gl_InvocationID] = vec4(n.xyz, 1.0);
}
