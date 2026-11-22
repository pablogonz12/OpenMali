#version 450
// gs_reject: the shape DXVK emits for a written-out HLSL pass-through GS -- a loop over the input vertices. Lane 1 only
// accepts a straight-line emit sequence (proof of the 0..n-1 order and count), so a loop is refused by name:
// "structured control flow". Unrolling it is the next widening of the classifier, not a silent acceptance.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 in_col[];
layout(location = 0) out vec4 v_col;
void main(){
  for (int i = 0; i < 3; i++) { gl_Position = gl_in[i].gl_Position; v_col = in_col[i]; EmitVertex(); }
  EndPrimitive();
}
