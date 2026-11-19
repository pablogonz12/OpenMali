# Cull distance: what we advertise and what we do (2026-11-19)

**Advertised:** `shaderCullDistance = true` only with `MALI_ADVERTISE_CULL=1`, and then
`maxCullDistances = 8`, `maxCombinedClipAndCullDistances = 8` (`maxClipDistances` is 8 either way).
**Behaviour when advertised:** every `gl_CullDistance` write is **ignored** — the outputs are demoted to
plain temporaries in `mali_compile.c` (`VARYING_SLOT_CULL_DIST0/1`), so a primitive that a conformant
driver would have culled is simply rasterised. Nothing faults and nothing renders wrongly; there is only
extra overdraw.

**This is not conformance.** `VK_EXT/CTS` cull-distance tests must stay excluded while it is on, and the
default build does not advertise it.

## Why it is not implemented

A cull distance is a *primitive*-level decision: cull the primitive iff every vertex has value < 0. The
vertex shader sees one vertex; the fragment shader sees interpolated values. Valhall has no cull stage,
so the only correct implementations are:

1. a compute pre-pass that reads the VS outputs of all vertices of each primitive and builds an index
   list without the culled triangles (the plan in `docs/cull-distance-plan.md`, PR #203/#205 territory);
2. CPU-side geometry emulation — not viable for a frame-time-sensitive driver.

## Relationship to #203 (`MALI_CULLDIST=1`, "cull as clip")

#203 lowers cull distances *as clip distances*. That is **non-conformant and not what this PR
advertises**: clip eliminates per-vertex (the primitive is clipped at the plane, not dropped), so a
triangle straddling 0 keeps a visible fragment where the spec says it disappears at the crossing, and a
whole culled triangle still contributes its clipped edge. It is an experiment, stays default-off, is
never combined with `MALI_ADVERTISE_CULL`, and no feature bit may be flipped while it is set. The
correct pair is: cull-as-clip is a *stopgap experiment*; advertise-drop (this change) is what the device
reports today.

## What a game sees

D3D11 `SV_CullDistance` is a hint used by tessellation/foliage shaders to save rasterisation. Ignoring
it costs fill rate, never correctness of the vertices that survive. Megabonk (Unity built-in RP) does not
emit `SV_CullDistance` at all. DXVK requires the feature bit at FL11_0; advertising it with the
ignore-behaviour is the smallest step that lets a device be created without lying about a shader's
outputs.

## Tests

- `tests/t_cull_shim.c`: both env states — feature false/limits 0 when off; with the env, feature true,
  `maxCullDistances` 8, the limits equal to the Vulkan minimums, and a device created with the feature.
- `tests/t_vk_feat --only cull` (fake backend: compile + draw path, pixels skipped) and
  `--only clip8`: `maxCullDistances` tracks the feature bit.
- Mutation: flipping `shaderCullDistance` without the `maxCullDistances` change (or the reverse) fails
  `t_cull_shim`; removing the demotion makes the cull shader compile to a clip-distance write, which
  `compile-guard`/`t_cull_shim` sizes catch with the version bump.
