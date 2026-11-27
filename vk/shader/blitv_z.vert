#version 450
// blitreplay (notes/blitreplay.md): RECONSTRUCTION of zink's u_blitter vertex shader for the black glBlitFramebuffer
// frame (notes/jobdump.md has the four vertices it feeds). The dumped module (spv-vert-2.spv, notes/uv.md) was never
// committed; this is its shape, not its bytes: attribute 0 = position (NDC), attribute 1 = texcoord in PIXELS, both
// passed through to one vec4 varying at location 0, whose .xy the blit FS truncates to integer texels and whose .w is
// the LOD. The frame's vertex data: v0 pos(-1,-1,0,1) tex(0,600,0,0), v1 pos(1,-1,0,1) tex(800,600,0,0), ...
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 tex;
layout(location = 0) out vec4 v;
void main(){ gl_Position = pos; v = vec4(tex.xy, 0.0, 0.0); }
