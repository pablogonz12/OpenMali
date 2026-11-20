#version 450
/* 8-set layout: the colour comes from the LAST set (7), so a draw passes only if the driver binds
 * and wires set 7 into the SRT, not merely sets 0..1 (maxBoundDescriptorSets is 8). */
layout(location=0) in vec4 v_col;
layout(location=0) out vec4 out_color;
layout(set=7, binding=0) uniform U7 { vec4 colour; } u7;
void main(){ out_color = u7.colour; }
