#version 450
// VK_EXT_transform_feedback probe: writes gl_Position and one streamed-out vector. The driver must refuse this
// pipeline with VK_ERROR_FEATURE_NOT_PRESENT while it has no streamout job (mali_tf.h / mali_compile.c).
layout(location = 0) in vec4 pos;
layout(location = 1, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out vec4 tf_out;
void main(){
  tf_out = pos;
  gl_Position = pos;
}
