#!/bin/sh
# interp_<mode>_<decor>[_inv].{vert,frag}.spv: the interpolation sweep of t_interp.
# One triangle with a per-vertex colour that cannot be confused between modes (v0 red, v1 green, v2 blue) and a
# per-vertex clip w (1, 2, 3), so perspective-correct and noperspective interpolation differ at every interior pixel
# and flat reads the provoking vertex. The qualifier is written identically in both stages (SPIR-V requires the
# interpolation decorations of a vertex output and the fragment input at the same Location to match); `invariant`
# only exists on the vertex output - glslang rejects it on a fragment input ("invariant is only allowed on output").
cd "$(dirname "$0")" || exit 1
for mode in smooth noperspective flat; do
   for decor in plain centroid sample; do
      aux=""
      [ "$decor" != plain ] && aux="$decor "
      for inv in 0 1; do
         iv=""
         [ "$inv" = 1 ] && iv="invariant "
         name=interp_${mode}_${decor}
         [ "$inv" = 1 ] && name=${name}_inv
         printf '#version 450\nlayout(location=0) %s%s out vec3 v;\nlayout(location=0) in vec2 p;\nlayout(location=1) in float w;\nlayout(location=2) in vec3 c;\nvoid main(){ v = c; gl_Position = vec4(p * w, 0.0, w); }\n' "$iv" "$aux$mode" > .interp_tmp.vert
         glslangValidator -V --target-env vulkan1.1 .interp_tmp.vert -o "$name.vert.spv" >/dev/null || exit 1
         if [ "$inv" = 0 ]; then
            printf '#version 450\nlayout(location=0) %s%s in vec3 v;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(v, 1.0); }\n' "$aux" "$mode" > .interp_tmp.frag
            glslangValidator -V --target-env vulkan1.1 .interp_tmp.frag -o "$name.frag.spv" >/dev/null || exit 1
         fi
      done
   done
done
rm -f .interp_tmp.vert .interp_tmp.frag
echo "interp: 18 vertex + 9 fragment SPIR-V modules"
