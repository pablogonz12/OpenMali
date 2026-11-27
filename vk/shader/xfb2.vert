#version 450
// VK_EXT_transform_feedback write probe: two buffers, buffer 0 from location 1, buffer 1 (bound at a nonzero
// offset) from location 2 with a 4-byte xfb_offset inside its 8-byte stride, so the test proves both the bind
// offset and the per-output offset are honoured rather than landing at address 0.
layout(location = 0) in vec4 pos;
layout(location = 1, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out vec4 tf_a;
layout(location = 2, xfb_buffer = 1, xfb_offset = 4, xfb_stride = 8) out float tf_b;
void main(){
  tf_a = pos;
  tf_b = pos.x * 2.0;
  gl_Position = pos;
}
