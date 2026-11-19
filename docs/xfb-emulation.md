# Transform feedback on Valhall/CSF without stream-out hardware

Status: **design + host-side prototype. VK_EXT_transform_feedback is NOT advertised** (not in
`vk/src/mali_physical_device.c`), and nothing in this document changes that. What exists today is a shader compile
variant, a capture plan (`vk/src/mali_xfb.c`) and offline tests (`vk/tests/t_xfb.c`); the command-stream launch and the
Vulkan entry points are not written yet.

## 1. The path we are copying: gallium panfrost on CSF

Mesa's gallium panfrost already does software transform feedback on Valhall, and it does it without any stream-out
hardware. Tracing it:

* `src/compiler/nir/nir_lower_xfb_to_stores.c` turns every `store_output` that carries `io_xfb` information into
  `store_global` at
  `xfb_address(buffer) + (instance_id * num_vertices + raw_vertex_id) * stride + offset`.
  It reads `xfb_address(buffer)` (64-bit) and `num_vertices` as system values. `load_vertex_id` is rewritten to
  `raw_vertex_id + raw_vertex_offset`.
* `src/gallium/drivers/panfrost/pan_shader.c:123-137` is the recipe: for a vertex shader with
  `has_transform_feedback_varyings` the driver sets `inputs.no_idvs = true` (**no IDVS for internal XFB shaders**) and
  runs `nir_opt_constant_folding`, `nir_io_add_intrinsic_xfb_info`, `nir_lower_xfb_to_stores` with
  `address_format = nir_address_format_64bit_global`.
* `src/gallium/drivers/panfrost/pan_cmdstream.c:3284` (`panfrost_launch_xfb`) is the orchestration: compile/select
  the *second* VA for the vertex shader (`vs_uncompiled->xfb`), emit its compute shader meta, and call
  `JOBX(launch_xfb)`. It runs **before** the real draw (`pan_cmdstream.c:3459`), then
  `panfrost_update_streamout_offsets()` advances the per-target byte offsets on the host. If nothing needs
  rasterisation (`panfrost_batch_skip_rasterization`), the real draw is skipped entirely.
* `src/gallium/drivers/panfrost/pan_csf.c:1373` (`csf_launch_xfb`) is the CSF job: workgroup size 1x1x1 with
  `allow_merging_workgroups`, `JOB_SIZE_X = count`, `JOB_SIZE_Y = instance_count`, `JOB_SIZE_Z = 1`,
  `cs_wait_slot(RENDER)` to order against the real draw, `cs_run_compute(1, MALI_TASK_AXIS_Z)`, then a
  `MALI_CS_FLUSH_MODE_CLEAN` / tiler `NONE` / `OTHER_FLUSH_MODE_INVALIDATE` flush so the stores are visible to the
  next job, and a wait on `SB_LS`.

Two properties matter and are worth stating, because they are what makes this cheap to mirror:

1. The XFB pass is **one invocation per captured output vertex**, with `instance_id * num_vertices + raw_vertex_id`
   giving a unique slot. No atomics, no append counter, no ordering between vertices.
2. It is a **compute** job (`cs_run_compute`), not a vertex job. There is no tiler, no IDVS, no rasterisation, and the
   results are ordinary scalar/vector global stores.

Known gallium gaps that we inherit as design constraints: indexed draws are a `TODO` in
`panfrost_launch_xfb`, and Mesa's `nir_lower_xfb_to_stores` has no notion of partial writes/counter clamping.

## 2. How that maps onto our pipeline

Our driver compiles offline through `vk/src/mali_compile.c` and submits pre-built binaries, so the mapping is:

| Mesa (gallium panfrost) | Ours |
|---|---|
| `inputs.no_idvs = true` | `mali_compile_opts.xfb` sets `in.no_idvs = true` |
| `nir_io_add_intrinsic_xfb_info` + `nir_lower_xfb_to_stores(64bit_global)` | same two passes, in the same order, after `nir_lower_io` (`mali_compile.c:495-506`) |
| `xfb_address(buffer)` system value | `load_push_constant` at `xfb_params_off + 8*buffer` of `struct mali_xfb_params` |
| `num_vertices` system value | `load_push_constant` at `xfb_params_off + 32` |
| `raw_vertex_offset` | constant 0 |
| `panfrost_emit_compute_shader_meta` + `JOBX(launch_xfb)` | not written yet: a compute launch of the xfb variant over `(output_vertices, instance_count)` with a 1x1x1 workgroup and workgroup merging, in front of the draw, with a cache invalidate after it |
| `panfrost_update_streamout_offsets` | `struct mali_xfb_plan` (`vk/src/mali_xfb.c`): counts and counter values are computed on the host from the draw alone |
| `panfrost_batch_skip_rasterization` | same: if the pipeline has rasterizer discard and only XFB, skip the real draw |

`struct mali_xfb_params` (40 bytes: four 64-bit buffer addresses, `num_vertices`, pad) is the FAU block the variant
reads; `xfb_params_off` must lie below `fau_reserved * 4`. The compute launch itself is the piece that
the GS-style kernel dispatch work (PR #241) provides: a shader that is launched as a kernel, not through IDVS, with
job size taken from the draw. Nothing in the shader compiler is blocked on it.

Gathering the Xfb decorations needed one non-obvious step. In SPIR-V a `gl_PerVertex` block carries
`XfbBuffer`/`XfbStride` on the *type* and `Offset` on its *members*; `nir_shader_gather_xfb_info` only walks whole
variables, so gathering before splitting silently captured nothing (this was the bug in the first draft: one
`store_global` for `v_col`, none for `gl_Position`). The Vulkan runtime does `nir_split_var_copies` then
`nir_split_per_member_structs` before gathering (`src/vulkan/runtime/vk_nir.c:323-345`); we now do the same, then
`nir_shader_gather_info` after the lowering so `writes_memory` and `inputs_read` are filled (PanVK calls it before
`pan_compile`; our offline path never did, which made the same shader look like it had zero attributes).

## 3. What of VK_EXT_transform_feedback can be honoured

The decisive property of the emulation is that with **one stream and one buffer** the byte position of every captured
vertex is a pure function of the draw: `slot = instance * num_vertices_per_instance + vertex_in_instance`. No GPU-side
append counter is needed, so the "recorded" count can be computed before the job is submitted and written to the
counter buffer by the host (or by the submission path) afterwards. Everything below follows from that.

| Feature | Honourable? | Why / what it costs |
|---|---|---|
| One stream, one buffer, interleaved | **yes, tested** | `vk/shader/xfb/basic.vert`: `gl_Position` at offset 0, `v_col` at 16, stride 32. `t_xfb` compiles the variant for v11 and v13 (2 global stores, no IDVS, 2 attributes read) and checks the recorded bytes and per-vertex data through the fake backend: `3 vertices recorded, 96 bytes, counter 96`, and nothing written past the count. |
| `CmdBeginTransformFeedbackEXT` / `CmdEndTransformFeedbackEXT` | **yes** | Begin captures `(buffer, offset, size)` and the counter value; End stores `start + written` and unbinds. Both are host bookkeeping in `struct mali_xfb_plan`. |
| Counter buffers, `pCounterBufferOffsets` at Begin/End | **yes** (host-side) | The recorded byte count is computed, not read back from the GPU, so it can be written into a counter buffer at any aligned offset. Requires that no other GPU work writes that range in the same submission, and that `counterBufferOffset` alignment is advertised conservatively (4 or 16 bytes, whichever the buffer path already guarantees). |
| Pause / resume (`EndTransformFeedback` then `BeginTransformFeedback` with the counter buffer) | **yes, tested** | The plan takes `start` = counter value at Begin, so the second draw continues at `start + written`; `t_xfb` covers the overflow case (room for 4 vertices → 1 whole triangle written, `needed` still 2, counter 96) and the resume case (7 slots, second draw writes 1 more triangle, counter 192). Note the Vulkan rule that a primitive which does not fit is not captured at all: `mali_xfb_plan` quantises to whole primitives, and every active buffer limits independently. |
| Overflow | **yes, tested** | `prims_written = min over active buffers of room / (stride * verts_per_prim)`; the query's `written` vs `needed` comes straight from that. |
| Indexed draws | **yes in principle, untested** | The variant runs per *output* vertex and reads attributes through the normal vertex fetch, so the same machinery covers indexed draws; but index-dependent primitive `.xfb` ordering and primitive restart need the index buffer in the plan. Primitive restart is refused today (`mali_xfb_plan` returns false). |
| Points, lines, line strips, triangle lists/strips/fans | **yes, tested for ordering** | `mali_xfb_input_vertex` follows the Vulkan primitive-assembly order (odd strip triangles swap the first two vertices; fans pivot on 0). Captured output is always whole primitives in assembly order. |
| Lines/triangles *streams rasterization selection* (`rasterizationStreamSelect`) | **no** | Needs geometry shaders, which we do not have; advertise `transformFeedbackRasterizationStreamSelect = false`. |
| Geometry shaders / multiple streams | **no** | We have no GS stage, so `geometryStreams = false`, `maxTransformFeedbackStreams = 1`. The plan is already keyed per buffer (`counter[]`, per-buffer stride), and `nir_lower_xfb_to_stores` handles up to 4 buffers, but a second *stream* has no producer. Streams other than 0 are refused. |
| Multiple buffers (not streams) | **plumbed, one tested** | `mali_xfb_plan` limits on the smallest active buffer and reports one counter per buffer; `mali_xfb_run_cpu` executes one buffer only. Interleaved multi-buffer is the natural next step and needs no new hardware feature — just more `store_global`s (the address base is already indexed by buffer). |
| `CmdDrawIndirectByteCountEXT` (zink's instanced XFB) | **not yet** | The launch size is a GPU-side value (`counter buffer` byte count / `vertexStride`), so the job size cannot be computed on the host without a stall. It needs (a) a tiny setup kernel that turns the byte count into a job size, or an indirect compute launch, and (b) `maxVertexCount` clamping inside the variant. Neither exists. Advertise `transformFeedbackDraw = false`, which is legal and makes zink fall back to non-indirect XFB (slow path, but correct). |
| Queries (`PRIMITIVES_GENERATED`, `TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN`, `TRANSFORM_FEEDBACK_OVERFLOW`, `STREAM_OVERFLOW`) | **yes** | All are derivable from `mali_xfb_plan` on the host; the overflow flags are `written < needed`. No GPU counter needed for the single-stream case. |
| `VkPipelineRasterizationStateStreamCreateInfoEXT` | **trivial** | Accept and validate `rasterizationStream == 0`. |

**The exact subset that could eventually be advertised** (and only after the launch path and entry points land and a
hardware run passes):

* `transformFeedback = true`, `geometryStreams = false`;
* `maxTransformFeedbackStreams = 1`, `maxTransformFeedbackBuffers = 1` (raise to 4 once multi-buffer is tested),
  `maxTransformFeedbackBufferSize =` the largest device allocation we bind, not `VK_WHOLE_SIZE`;
* `transformFeedbackCounterBuffers = 1`, `transformFeedbackCounterResetEnable = true`,
  `transformFeedbackCounterBufferOffsetAlignment` = the counter BO alignment we prove (at least 4);
* `transformFeedbackPrimitivesWritten`, `transformFeedbackStreamsLinesTriangles = true`,
  `transformFeedbackRasterizationStreamSelect = false`, **`transformFeedbackDraw = false`**;
* no geometry shaders, no tessellation in the same pipeline.

Not advertised today: the extension name is absent from the device's extension list and the features struct is not
chained. A pipeline that asks for XFB must keep failing `VK_ERROR_FEATURE_NOT_PRESENT`.

## 4. Cost

The measured numbers in `t_xfb` are **CPU model artifacts, not hardware estimates**. The bench runs the C twin of the
shader (a handful of loads, 8 multiplies/adds) against the same twin plus a `memcpy` of the 32-byte record, on the
host, and reports roughly `0.4 ns/vertex` against `4.3 ns/vertex` (x11). That ratio is dominated by the host's
`memcpy`/function-call overhead and says nothing about Mali: **it is not a projection for a 120 fps UI and must not be
used as one.**

What the hardware cost actually is, structurally:

* one extra vertex-shader invocation per captured output vertex, in a compute job. Per instance the count is
  `prims * verts_per_prim`, not the draw's vertex count, so triangle strips inflate it (2n-2 captured vertices for n
  inputs) and primitive restart/degenerate triangles are the caller's problem;
* per invocation, `1..4` global stores of `stride` bytes each (32 in the tested case), plus the attribute fetches and
  ALU of the vertex shader, which the plain draw pays anyway. The capture pass therefore roughly doubles the vertex
  work of a draw that is already VS-bound, and adds a second set of attribute loads (no IDVS means no shared
  intermediate);
* no tiler, no rasterisation, no atomics, and the stores are contiguous 32-byte writes — the friendly case for the
  memory system. A 100k-vertex UI draw is a few MB of writes, i.e. well under a millisecond of bandwidth, but that is
  an argument, not a measurement.

The real measurement to do, once a phone is free, is a paired hardware run: the *same* draw (same vertex count, same
shader, same buffers) submitted twice — once as a VS-only draw, once with the XFB compute pass in front of it — timed
with GPU timestamps if available and with frame time otherwise, at a few vertex counts (10k, 100k, 1M) and for both
`rasterizerDiscard` (capture only) and normal rasterisation (capture + draw). Report ns per captured vertex and the
delta as a fraction of a 8.3 ms frame. Only that number may be used to decide whether a 120 fps UI survives it.

## 5. Version and compatibility

`MALI_COMPILE_VERSION` is **not bumped**: every existing shader compiles byte-identically (all new code is behind
`o->xfb`, and the cache key already includes `xfb` and `xfb_params_off`), so no cached shader is invalidated and no
shader that did not ask for the variant changes. The bump should happen if and when the variant's own output changes
in a way that matters to a cached entry.

## 6. Test surface

`vk/tests/build-t_xfb.sh` (offline, no phone):

* `test_plan`: counts, overflow quantisation, pause/resume, strips/fans ordering, instances, two buffers, and the
  refusals (primitive restart, adjacency, unaligned stride, nothing captured);
* `test_data`: the CPU twin writes the exact 32-byte records and touches nothing past the recorded count;
* `test_fake_backend`: the same capture into a BO allocated by `mali_backend_fake`, read back through `va_cpu` —
  `3 vertices recorded, 96 bytes, counter 96`;
* `test_compile(11|13)`: the real variant compiles for both architectures, has global stores, no IDVS, and reads the
  two attributes (v11 `384 B` plain / `256 B` variant; v13 the same);
* `bench`: the CPU twin ratio, labelled as a model artifact (see §4).

Still missing before any of this is usable: the compute launch in the command stream, the Vulkan entry points and
feature plumbing, multi-buffer capture, and the `DrawIndirectByteCountEXT` setup kernel.
