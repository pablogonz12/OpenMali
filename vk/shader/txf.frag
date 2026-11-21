#version 450
// txf-fix: our own plain texelFetch of a combined image sampler at the frame's layout (set 2, binding 128), the
// operation zink's blit FS (blitfs_z.frag.spv) is: OpImageFetch with the LOD from the varying. Same descriptor,
// same varying as blitfs_o.frag, so the only difference to that row is TEX_FETCH vs TEX.
layout(location = 0) in vec4 v;
layout(set = 2, binding = 128) uniform sampler2D tex;
layout(location = 0) out vec4 out_color;
void main(){ out_color = texelFetch(tex, ivec2(v.xy), int(v.w)); }
