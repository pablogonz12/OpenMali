#version 450
// gs_reject: writes gl_PrimitiveID into a varying. Our vertex program has no primitive-id source (no prim_id sysval),
// so this is refused by name rather than reading garbage: "reads gl_PrimitiveID".
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) out vec4 v_col;
void main(){
  gl_Position = gl_in[0].gl_Position; v_col = vec4(float(gl_PrimitiveID)); EmitVertex();
  gl_Position = gl_in[1].gl_Position; v_col = vec4(float(gl_PrimitiveID)); EmitVertex();
  gl_Position = gl_in[2].gl_Position; v_col = vec4(float(gl_PrimitiveID)); EmitVertex();
  EndPrimitive();
}
