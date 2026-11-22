#version 450
// robustImageAccess (zink GL 4.3, notes/zink-gl-version.md:50-56): an out-of-bounds imageLoad must return zero, with (0,0,1)
// inserted for the missing components of the format. R8_UNORM reads as (r,0,0,1) and the spec value of a missing alpha is 1,
// so every reading of the rule gives (0,0,0,1) here. The in-bounds load proves the image bound is the one filled, so a zero
// from the far load is not just an empty image.
layout(set=0, binding=0, r8) uniform readonly image2D img;
layout(set=0, binding=1) buffer B { uint oob[4]; uint ib[4]; uint neg[4]; } b;
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 o;
void main(){
  vec4 a = imageLoad(img, ivec2(1000, 1000));   /* far outside a 64x64 image */
  vec4 c = imageLoad(img, ivec2(3, 3));
  vec4 n = imageLoad(img, ivec2(-5, -5));   /* the other side of the bounds */
  uvec4 au = uvec4(round(a * 255.0));
  uvec4 cu = uvec4(round(c * 255.0));
  uvec4 nu = uvec4(round(n * 255.0));
  if (gl_FragCoord.x < 2.0 && gl_FragCoord.y < 2.0) {
    b.oob[0] = au.r; b.oob[1] = au.g; b.oob[2] = au.b; b.oob[3] = au.a;
    b.ib[0] = cu.r; b.ib[1] = cu.g; b.ib[2] = cu.b; b.ib[3] = cu.a;
    b.neg[0] = nu.r; b.neg[1] = nu.g; b.neg[2] = nu.b; b.neg[3] = nu.a;
  }
  o = vec4(1.0);
}
