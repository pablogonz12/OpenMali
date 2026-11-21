#version 450
// blitreplay: our own vertex shader (the twin's VS). Same varying semantics as blitv_z.vert (pixel coordinates of the
// 800x600 target, y up) but computed from the position attribute instead of passed through from attribute 1, so the
// twin differs from the replay only in shader code and not in what crosses the varying.
layout(location = 0) in vec4 pos;
layout(location = 0) out vec4 v;
void main(){
   gl_Position = vec4(pos.xy, 0.0, 1.0);
   v = vec4((pos.x * 0.5 + 0.5) * 800.0, (0.5 - pos.y * 0.5) * 600.0, 0.0, 0.0);
}
