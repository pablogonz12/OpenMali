#version 450
// blitreplay: our own fragment shader (the twin's FS, and the FS of the bisect variants that move the binding/set).
// It gets the same pixel-coordinate varying as the replay (blitv_z.vert) and normalises it for a plain texture()
// sample instead of zink's texelFetch, i.e. it is our normal path through the same descriptor.
#ifndef FS_SET
#define FS_SET 2
#endif
#ifndef FS_BIND
#define FS_BIND 128
#endif
layout(location = 0) in vec4 v;
layout(set = FS_SET, binding = FS_BIND) uniform sampler2D tex;
layout(location = 0) out vec4 out_color;
void main(){ out_color = texture(tex, v.xy * vec2(1.0 / 800.0, 1.0 / 600.0)); }
