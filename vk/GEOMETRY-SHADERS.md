# Geometry shaders (experimental)

Mali's tiler has no geometry stage, so a geometry shader is lowered to **two compute kernels plus a generated
draw**:

1. the application's VS is recompiled as a compute kernel (`vs_k`) that runs once per vertex, fetches the vertex
   buffer itself (no IDVS) and writes one record per vertex (`vs_records`, 16 bytes per written VS output);
2. the GS body becomes a compute kernel (`gs_k`), one invocation per input primitive, that reads those records,
   runs the body, and writes one 16-byte record per emitted vertex plus a u32 index per emitted vertex into its
   slot of the index buffer;
3. the draw is re-issued as an **indexed triangle-strip/point-list draw** whose VS is a generated passthrough
   shader that turns index → record → VS outputs. The index buffer is padded with `0xffffffff` (primitive
   restart) so unwritten slots and vertices past `max_vertices` terminate their primitive instead of pulling
   another slot's records.

Both kernels and the passthrough VS are compiled by the existing back end (`mali_compile_nir`), recorded in one
code BO, and dispatched at frame build: the kernels run over their own arena ranges (`mali_frame_build`), the
draw then walks the index buffer.

## Enabling

`MALI_GS_EXPERIMENTAL=1` in the environment. Nothing is advertised: `geometryShader` stays `VK_FALSE` and the
feature is neither in `VkPhysicalDeviceFeatures` nor accepted without the variable.

## Supported

| Thing | State |
|---|---|
| Input primitives | points, lines, triangles (lists only) |
| Output primitive | point list, line strip, triangle strip |
| Invocations | 1 (`invocations > 1` is refused at pipeline creation) |
| `max_vertices` | honoured; extras are dropped, no spill into the next slot |
| Emitting nothing | supported: the slot stays all-restart and draws no fragments |
| `EndPrimitive` / restart padding | supported (one index per emitted vertex, padded to `2*vertices_out`, min 4) |
| VS system values | `gl_VertexIndex` / `gl_VertexID`, `gl_InstanceIndex` = 0 (one instance only) |
| Vertex input | per-vertex `R32{,G32,B32,A32}_SFLOAT`, up to 4 bindings and 16 attributes, full 32-bit float components |
| Uniforms / SSBOs in the VS or GS | through the normal descriptor sets (both kernels get the draw's SRT) |
| Push constants | the application's block at 0, `struct mali_gs_params` at `pc_off` (8-byte aligned, params 96 B, whole block ≤ 128 B) |
| Outputs | one 16-byte slot per written output, popcount-ranked (same layout as PanVK's prerast records) |
| Fragment stage | normal; linked against the passthrough VS (`p->gs.pt.layout`) |
| Draws | `vkCmdDraw` |

## Refused (pipeline creation → `VK_ERROR_FEATURE_NOT_PRESENT`)

- input primitive types `*_STRIP`, `*_ADJACENCY`, patches (no strip assembly, no adjacency data)
- output primitive types other than point list / line strip / triangle strip (`lines`/`triangles` as such,
  `triangle_strip` with adjacency, `*_STRIP_ADJACENCY`)
- `invocations > 1`
- vertex attributes that are not per-vertex or not 32-bit float (the record kernel does not convert formats)
- more than 4 vertex bindings or 16 attributes
- push constants whose size plus the params block exceeds the 128-byte push block
- `triangle_strip` with `primitive_restartEnable` from the application is ignored — restart is the driver's own
  padding

## Refused at draw time (`VK_ERROR_FEATURE_NOT_PRESENT` on the command buffer)

- `instanceCount != 1` (one kernel sweep per instance is not implemented; drawing one instance would be wrong)
- `vkCmdDrawIndexed` and the indirect draws (`vkCmdDraw*Indirect*`)
- `vkCmdDraw` with fewer vertices than one input primitive: planned empty, no dispatch and no draw

## Not implemented (would be needed for a general feature)

- **Line/triangle strips as input primitives.** The kernel maps one thread to one *list* primitive; strip
  assembly needs the parity-dependent winding and `gl_PrimitiveID` semantics.
- **Adjacency** input/output primitives and `gl_InvocationID`/`invocations > 1`.
- **Array outputs** (`out vec4 x[]`) and per-vertex output indexing other than a constant.
- **Instancing**: the VS kernel reads `params.first_vertex` only; `InstanceIndex` is 0.
- **`noperspective` interpolation.** The driver lowers `load_noperspective_varyings_pan` to 0 for the
  generated passthrough VS (PanVK inlines the linked FS mask), so every varying interpolates
  perspective-correct; the FS side is not lowered either.
- **Streams** (`EmitStreamVertex`), `gl_PrimitiveIDIn` beyond the slot index, `EndStreamPrimitive`.
- **Vertex formats** other than 32-bit float; per-instance attributes; robustness/clamping.
- **Real-hardware validation**: everything above is tested on the host (NIR interpreter + CPU rasteriser,
  `tests/t_gs.c`); the command stream has not been run on a phone in this slice. The compute → tiler ordering
  relies on the per-kernel `WAIT` + L2 flush the frame builder already uses for compute jobs.

## Tests

`tests/build-t_gs.sh` (in `check.sh`'s standalone list) runs `t_gs` on v11 and v13:

- analyse + record layout + passthrough (slices 1-2, unchanged),
- triangle → quad and point → quad through `mali_gs_pipe_build` + `mali_gs_plan_draw`, asserting live index
  counts, the generated strip and per-pixel colour/uv on the CPU rasteriser,
- emit-nothing: zero live indices and zero covered pixels; a triangle draw with 2 vertices is planned empty,
- plan mutation: walking only `prims * vertices_out` indices instead of the padded stride loses the second
  slot's quad (pixels vanish), so the padding count is load-bearing,
- truncation mutation (slice 2): dropping the `max_vertices` bound is detected.

The padding itself is also defensive: the kernel restart-fills its own slot before emitting, so an unwritten
slot (or a thread the dispatch never reached) still terminates.
