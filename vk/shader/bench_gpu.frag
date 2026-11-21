#version 450
/* Fill benchmark fragment shader: 12 octaves of value noise (48 sin() per pixel).
 * Constant loop bounds and no time uniform: a frame is bit-identical on every run. */
layout(location = 0) out vec4 o;

float hash(vec2 p)
{
   return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p)
{
   vec2 i = floor(p);
   vec2 f = fract(p);
   vec2 u = f * f * (3.0 - 2.0 * f);
   return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
              mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

void main()
{
   vec2 uv = gl_FragCoord.xy / vec2(512.0);
   float a = 0.0;
   float amp = 0.55;
   for (int i = 0; i < 12; ++i) {
      a += amp * noise(uv * 24.0 + vec2(float(i) * 5.7, float(i) * 3.1));
      uv = uv * 1.87 + vec2(0.37, 0.61);
      amp *= 0.62;
   }
   o = vec4(a, a * 0.75, 1.0 - a, 1.0);
}
