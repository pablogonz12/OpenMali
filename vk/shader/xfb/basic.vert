#version 450
// xfb emulation test: one stream, one buffer, vec4 position (offset 0) + vec4 varying (offset 16), stride 32.
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_col;
layout(push_constant) uniform PC { vec4 scale; } pc;
layout(xfb_buffer = 0, xfb_stride = 32) out gl_PerVertex {
   layout(xfb_offset = 0) vec4 gl_Position;
};
layout(location = 0, xfb_buffer = 0, xfb_offset = 16, xfb_stride = 32) out vec4 v_col;
void main() {
   gl_Position = in_pos * pc.scale;
   v_col = in_col + vec4(float(gl_VertexIndex), 0.0, 0.0, 0.0);
}
