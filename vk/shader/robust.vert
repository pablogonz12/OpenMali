#version 450
// Robust-access probe: vertex-buffer OOB fetch. Draws 16 point vertices from an 8-word vertex buffer; vertex i lands on pixel i of a 16x1 R32_UINT target.
layout(location = 0) in uint v;
layout(location = 0) flat out uint ov;
void main()
{
   ov = v;
   gl_Position = vec4((float(gl_VertexIndex) + 0.5) / 8.0 - 1.0, 0.0, 0.0, 1.0);
   gl_PointSize = 1.0;
}
