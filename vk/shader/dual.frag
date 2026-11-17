#version 450
// dualSrcBlend test: o0 = interpolated colour, o1 = constant 0.5 (blend factors SRC1_COLOR / SRC1_ALPHA read it)
layout(location=0) in vec4 v_col;
layout(location=0, index=0) out vec4 o0;
layout(location=0, index=1) out vec4 o1;
void main(){ o0 = v_col; o1 = vec4(0.5); }
