#version 450
layout(location = 0) in vec2 st;
layout(location = 1) flat in int idx;
layout(location = 0) out vec4 o;
void main() {
   o = vec4(dFdx(st.x), dFdy(st.y), fwidth(st.x + st.y) * 0.5, 0.0) * (128.0 / 255.0);
   o.a = float(idx) / 255.0;
}
