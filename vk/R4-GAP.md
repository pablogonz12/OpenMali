# R4 gap analysis: our driver vs DXVK / vkd3d-proton (2026-11-17, offline)

Sources: DXVK master (v3.1.1 is the latest tag), `src/dxvk/dxvk_device_info.cpp` `DxvkDeviceCapabilities` feature table (fetched to
`/tmp/r4scratch/dxvk/info.cpp`; `featureRequired=true` entries below). vkd3d-proton and Megabonk/Steam UI needs are from knowledge, NOT fetched:
verify when online (see "Unverified").

## 1. What our driver exposes today (read from `vk/src`)

- Device `apiVersion` = 1.0 (`MALI_DEVICE_API_VERSION`, mali_private.h:66); instance 1.3 (mali_private.h:65).
- Device extensions (mali_physical_device.c:124-136): KHR_timeline_semaphore, KHR_descriptor_update_template, KHR_external_memory(+_fd), EXT_external_memory_dma_buf,
  KHR_external_semaphore(+_fd), KHR_external_fence(+_fd), KHR_swapchain(+mutable_format), EXT_image_drm_format_modifier (LINEAR only).
- Features: `vk_features` = `{timelineSemaphore}` only: every core 1.0 feature is false (no robustBufferAccess, no samplerAnisotropy, no BC, no geometryShader...).
- Limits: all 1.0 required minimums (maxImageDimension2D 4096, maxColorAttachments 4, maxBoundDescriptorSets 4, maxPerStageDescriptorSamplers 16,
  maxPushConstantsSize 128, maxViewports 1, maxSamplerAnisotropy 1, no subgroup props, no 1.1/1.2/1.3 property structs filled).
- Queue: 1 family (G|C|T), `timestampValidBits = 0`.
- Entrypoints implemented (mali_*): Create/Destroy buffer/image/view/sampler/pipeline (graphics, compute)/descriptors/query pool, Cmd Bind*/Draw/DrawIndexed/Dispatch(+Indirect)/
  Copy*2/Blit2/Resolve2/Clear*/Fill/Update/PushConstants/Begin+EndQuery/WriteTimestamp2/PipelineBarrier2/BeginRendering/EndRendering, host query reset, memory map.
  Missing: all indirect draws (DrawIndirect, DrawIndexedIndirect, *Count), SetViewport/Scissor etc. beyond what the dynamic-state path has (check mali_cmd.c), SetEvent2/WaitEvents2
  (events are in mali_event.c, check wiring), push descriptors, CmdBeginRenderPass2 (should come from `vk_render_pass`), vkGetBufferDeviceAddress, multi-draw, transform feedback.
- WSI exists (M14 wiring), Wayland platform.

## 2. PanVK oracle on Poco

No vulkaninfo dump of PanVK is in the repo (`grep` of runs/, docs/, FINDINGS*: only "Vulkan 1.3.278 (Mali-G925 proprietary)" for the blob and an unlabeled "Vulkan 1.4 on v10-v14 ... lacks geometry/tessellation,
fillModeNonSolid, multiViewport" line, which is the PanVK capability statement). Only the dEQP baseline exists (`deqp/baseline-poco.tsv`: 6800 info/device_init/object_management cases, 6278 pass, 385 NotSupported).
**Action R4-M0: run `vulkaninfo --json` for PanVK on Poco+CMF through qdirect/guest and commit it as `deqp/oracle-panvk-{poco,cmf}.json`.**
Takeaway from that statement: PanVK itself has NO geometryShader, NO tessellation, NO fillModeNonSolid, NO multiViewport => the DXVK-required set is not reachable by a straight PanVK clone;
the R5 emulations (GS/tess/xfb) are needed even to pass DXVK's device check, so R4 must expose geometryShader early (stub that compiles but is flagged as emulated) or run DXVK with a patched
requirement list (DXVK_CONFIG / a local patch: DXVK master marks geometryShader `true` at info.cpp:867).

## 3. DXVK required (hard fail if missing) vs our state

| Item | Req. by | Ours | Mesa runtime gives | Effort (agent-days) | Blocks instance/device creation? |
|---|---|---|---|---|---|
| API 1.3 device (DxvkVulkanApiVersion check, info.cpp:115) | DXVK | 1.0 | `vk_properties`/`vk_features` tables, `vk_common_*` fallbacks for 1.1/1.2/1.3 entrypoints (EnumerateInstanceVersion, GetDeviceQueue2, Bind*Memory2, CreateRenderPass2 -> vk_render_pass, CmdPipelineBarrier->sync2, queue submit2 via vk_queue) | 1 (flip + fill structs) + whatever entrypoints are then required | YES (first) |
| core: depthBiasClamp, depthClamp, dualSrcBlend, fillModeNonSolid, fragmentStoresAndAtomics, fullDrawIndexUint32, imageCubeArray, independentBlend, multiDrawIndirect, multiViewport, occlusionQueryPrecise, robustBufferAccess, sampleRateShading, samplerAnisotropy, shaderClipDistance, shaderCullDistance, shaderImageGatherExtended, shaderInt16, shaderInt64, shaderSampledImageArrayDynamicIndexing, textureCompressionBC | DXVK `true` | all false | NIR lowering exists for most (int64 lower, clip/cull as varyings); BC decode = software (below) | each 0.5-3, total ~14 (see ranking) | YES (device rejected) |
| geometryShader | DXVK | none (HW has none) | `nir_lower_gs_intrinsics`, `nir_lower_io_to_scalar`; panfrost has no GS path | 10-12 (R5 item; pull forward as VS+compute emulation) | YES |
| vk11: shaderDrawParameters, storageBuffer16BitAccess | DXVK | no | spirv_to_nir caps; HW has 16-bit load/store | 1.5 | YES |
| vk12: bufferDeviceAddress, descriptorIndexing + SampledImage UpdateAfterBind / UpdateUnusedWhilePending / PartiallyBound / runtimeDescriptorArray, storageBuffer8BitAccess, hostQueryReset, samplerMirrorClampToEdge, scalarBlockLayout, shaderInt8, timelineSemaphore (DONE), uniformBufferStandardLayout, vulkanMemoryModel | DXVK `true` | only timeline | `vk_sync_timeline` (done); host query reset is ours already; BDA: kbase GPU VA = host VA mapping, need `mali_GetBufferDeviceAddress` (0.5); indexing: our descriptor lowering (mali_descriptor_lower.c) is "flat" (mali_pipeline_layout_flat.h): needs real variable-count / unbounded arrays + update-after-bind pools | 6-8 for indexing, 1 for BDA, 1 for rest | YES |
| vk13: inlineUniformBlock, computeFullSubgroups, dynamicRendering (BeginRendering exported already), maintenance4 (Get*MemoryRequirements exist), shaderDemoteToHelperInvocation, shaderZeroInitializeWorkgroupMemory, subgroupSizeControl, synchronization2 | DXVK `true` | Cmd*2 barrier/timestamp exist; the rest not advertised | vk_common sync2 wrappers; `nir_lower_zero_init`; demote is `nir_demote` in Valhall backend (check); subgroups: Valhall warp = 16 lanes (G615/G925: 16, verify) | ~6 | YES |
| EXT_depth_clip_enable | DXVK true | no | `vk_rasterization_state` parses it; emission is a CS field | 1 | YES |
| EXT_robustness2: robustBufferAccess2, nullDescriptor | DXVK true | no | `vk_robustness` NIR lowering; nullDescriptor needs a defined null descriptor in our descriptor encoder | 3-4 | YES |
| KHR_maintenance5, KHR_maintenance6 | DXVK true | no | maintenance5 (CmdBindIndexBuffer2, GetRenderingAreaGranularity, DeviceImageSubresourceLayout, vk_common for most), maintenance6 (BindMemory2 pNext, push-descriptor/descriptor set 2 variants, vk_common) | 1.5 | YES |
| KHR_load_store_op_none (core in dynamic rendering 1.4, ext now) | DXVK true | no | vk_render_pass handles semantics | 0.5 | YES |
| KHR_swapchain | DXVK true | yes | wsi_common | done | no |

DXVK optional (affects behaviour, not startup): EXT_graphics_pipeline_library (+pipeline_library; big stutter win; needs vk_graphics_pipeline_state library flags + our linker; 6-8),
EXT_extended_dynamic_state3 subset (AlphaToCoverageEnable, DepthClipEnable, RasterizationSamples, SampleMask, LineRasterizationMode, SampleLocationsEnable; the vk_dynamic_graphics_state tracks them, emission ours; 3),
EXT_custom_border_color (+border_color_swizzle; vk_sampler parses; Mali has 4 border color slots in the sampler descriptor? check; 2),
EXT_transform_feedback (+geometryStreams; D3D11 SO; emulate by VS + store to buffer; R5; 8), EXT_vertex_attribute_divisor (1),
EXT_memory_budget/priority (1), EXT_non_seamless_cube_map (0.5, HW support unverified), EXT_multi_draw (1), EXT_line_rasterization (2), EXT_fragment_shader_interlock (skip),
EXT_sample_locations (standardSampleLocations true; 1), KHR_dynamic_rendering_local_read (tiler win, 2), EXT_shader_stencil_export (check HW, 1), EXT_attachment_feedback_loop_layout, EXT_swapchain_maintenance1 /
KHR_present_id/wait (1 each, WSI), KHR_calibrated_timestamps (0.5), KHR_device_fault, KHR_maintenance7..11, EXT_descriptor_buffer / descriptor_heap (skip; neither HW-friendly).

## 4. vkd3d-proton (R7, from memory; verify)

Hard requirements in addition to the above: Vulkan 1.3 (1.4 recently preferred), `descriptorIndexing` incl. all UpdateAfterBind kinds and runtimeDescriptorArray,
`maxPerStage*` limits >= 1,000,000 / 1,048,576 (or the DXVK-style descriptor heap path), `bufferDeviceAddress`, `timelineSemaphore`, `dynamicRendering`, `synchronization2`,
`shaderDrawParameters`, `EXT_mutable_descriptor_type`, `KHR_push_descriptor`, `EXT_extended_dynamic_state{,2,3}`, `EXT_robustness2`, `EXT_shader_demote..`, `shaderInt64`, `shaderFloat16/Int8`,
`KHR_shader_float_controls`, `KHR_shader_subgroup_extended_types`, `EXT_image_view_min_lod`, `KHR_ray_tracing` optional, mesh shaders optional. Needs our descriptor model to become bindless-capable
(descriptor heaps with VA-based tables), which the "flat" layout in `mali_pipeline_layout_flat.h` does not support. Defer to R7; but design R4 descriptor indexing so it extends.

## 5. Megabonk (Unity) and Steam UI (not verified)

Megabonk is a Unity game; on Linux/Proton Unity's default is D3D11 -> DXVK (so DXVK feature set above = Megabonk). If the Unity Vulkan backend is chosen instead it needs
core 1.1+ basics (samplerAnisotropy, imageCubeArray, depthClamp, shaderClipDistance, BC, tessellation optional, geometryShader NOT required by default) and is cheaper. Steam UI
(CEF/Chromium, Skia Ganesh/Graphite via Vulkan or ANGLE/GL): Vulkan 1.1 + external memory/semaphore fd, dma-buf image drm format modifier, ycbcr conversion (SamplerYcbcrConversion, core 1.1,
vk_common has it), `KHR_maintenance*` small set, sync fd; no GS/tess. It is the early R4 target.

## 6. Ranking by blocking power for "DXVK reaches a menu" (R4 order)

| # | Item | Why / unblocks | Mesa free | Effort |
|---|---|---|---|---|
| 1 | Device API 1.3 + fill `vk_properties` 1.1/1.2/1.3 (subgroup size 16, sizes, driverID, conformance version) + advertise vk_features honestly | Without it DXVK exits at once | tables | 2 |
| 2 | Entry-point completeness for 1.1-1.3 (vk_common wrappers, GetDeviceQueue2, BindImageMemory2 present, Create/Cmd RenderPass2 via vk_render_pass, CmdDrawIndirect*, Cmd SetViewport/... all dynamic) | loader/DXVK call these | vk_common_* | 3 |
| 3 | Core 1.0 feature block (anisotropy, depthClamp+bias, cubeArray, independentBlend, dualSrcBlend, fillModeNonSolid, multiViewport, clip/cull, Int16/Int64, gather, sample-rate shading, fragmentStoresAndAtomics, multiDrawIndirect) | device check | partly NIR | ~10 |
| 4 | shaderInt64/Int16/Int8 + 16/8-bit storage + draw parameters + scalar layout + vulkan memory model + zero-init + demote + subgroup basic/vote/ballot/arith (DXVK uses subgroup ops in some shaders) | features + shader correctness | NIR lowering | 5 |
| 5 | robustBufferAccess + robustness2 (nullDescriptor) | check + correctness | `nir_lower_robust_access` | 4 |
| 6 | descriptor indexing set (update-after-bind, partially bound, runtime array, unbounded) + inline uniform blocks | DXVK uses for bindless-ish resources | `vk_descriptor_set_layout` | 7 |
| 7 | BDA | check | none, GPU VA = ours | 1 |
| 8 | BC1-7 + ETC2/ASTC + others: Poco G925 has ASTC/ETC2 HW but BC? Mali has no BC HW. Decode to RGBA8/RGBA16F at upload (copy path compute shader) or on view creation | textureCompressionBC true is required; also all D3D11 games upload BC | none (PanVK exposes BC? only on some) | 8 |
| 9 | geometryShader (emulated or at least "reported + compile path"), tessellation | DXVK GS required; R5 | none | 10 |
| 10 | depth_clip_enable, maintenance5/6, load_store_op_none, ext dyn state, custom border colour, vertex divisor | DXVK startup + correctness | vk_graphics_state | 6 |
| 11 | EXT_graphics_pipeline_library | stutter, not startup | partial | 7 |
| 12 | transform feedback emulation | D3D11 SO games | none | 8 |

Totals: items 1-8 ~40 agent-days; 9-12 ~31 (mostly R5). Ladder budget in the proposal is 8 agent-weeks for R4 = 40 days: items 1-8 + 10 fits only if descriptor model and BC are 40% reuse; plan has 48 d.

## 7. Unverified / risks

- vkd3d-proton limits and Megabonk renderer: not fetched (offline brief). Verify with `strings Megabonk_Data/Plugins/*.dll` / launch log (`-force-vulkan`, `Player.log`).
- Whether Valhall G615/G925 samples BC (it does not per our info; Mesa's panfrost exposes BC through `PAN_ARCH >= 10 ... format tables`? check mali_format.c).
- Cold subgroup width and `subgroupSizeControl`: min=max=16 on both v11 and v13 (to confirm from compiler output).
- DXVK master (3.x) tightened requirements (maintenance5/6 required, GS required). Older 2.x releases (e.g. v2.5) required less (no maintenance5/6 hard require). A DXVK version pin of 2.6/2.7 shortens R4 by ~3 days.
- No oracle dump: item R4-M0.

## BC (R4-M7, offline part done 2026-11-17)
Design: a BC image owns the compressed data in its real block layout plus a hidden linear shadow in the same allocation (compressed region, then shadow at `shadow.base`).
Compute shaders decode compressed -> shadow after every write to the compressed data; views sample the shadow. Shadow formats: BC1/2/3/7 R8G8B8A8 (UNORM/SRGB), BC4 R8, BC5 R8G8 (U/SNORM),
BC6H R16G16B16A16_SFLOAT (alpha 1.0). Shadow rows padded to 4x4 blocks, 64 B aligned. One job per region, 1 invocation per block, 2 SSBOs, 48 B push constants.
Features advertised (optimal tiling only): SAMPLED, FILTER_LINEAR, BLIT_SRC, TRANSFER_SRC, TRANSFER_DST. No storage/attachments/1D/MSAA/linear/BLOCK_TEXEL_VIEW_COMPATIBLE.
- Files (new, nothing existing edited): `src/mali_bc.[ch]` (formats, layout, decode planner, host reference `mali_bc_ref_run`, SPIR-V table), `src/mali_bc_spv.h`, `shader/bc1.comp bc4.comp bc6h.comp bc7.comp`
  (+ `.spv`; regenerate with `shader/gen-bc-spv.sh`, tables by `gen-bc-tables.py`, BC6H/BC7 from `bc*.comp.in`), `shader/bc_probe.comp`, `shader/golden_bc.h` (generated by `t_bc --gen-golden <abs path>`).
- Tests: `tests/build-t_bc.sh` builds and runs `t_bc` (formats, layout, planner, reference vs Mesa `util_format_*` decoders, the real SPIR-V on the host Vulkan GPU, `mali_compile` v11/v13 limits + golden;
  3236 checks, 0 failures) and builds `t_bc_hw`. Mesa 26.3 has no BC decode in `vk_texcompress_*`, so shaders are from scratch (BC6H/BC7 tables from `texcompress_bptc_tmp.h`).
- Compile stats (v11 = v13): bc1 13.8 KB 64 regs, bc4 5.7 KB 64 regs, bc6h 18 KB 32 regs + 2432 B TLS, bc7 15 KB 32 regs + 1504 B TLS; WLS 0, local 64x1x1, push <= 40 B. TLS = local arrays: check spill cost on device.
- Wiring needed (one worker owning `vk/src`):
  1. `mali_physical_device.c`: `textureCompressionBC = true`; `mali_format.c`: BC formats report `mali_bc_optimal_features()`; `mali_bc_image_supported()` in `GetPhysicalDeviceImageFormatProperties`.
  2. `mali_image.c`: create/memory requirements from `mali_bc_layout_init` (size, alignment); store the layout in the image; view over a BC image binds the shadow (`shadow` format, `shadow.base` + per-level offsets), not the compressed data.
  3. After `vkCmdCopyBufferToImage`, image-to-image copy into a BC image, and `vkCmdUpdate...`: call `mali_bc_plan_decode` / `_all`, append the decode jobs to the same ops list (barrier after the copy, before sampling).
  4. `mali_meta.c`: materialise path for shader ids >= MALI_META_NSHADERS (bc1/bc4/bc6h/bc7 via `mali_bc_spirv`, compile with `fau_reserved = push/4`, 2 storage-buffer set, as `shader_get_locked`).
  5. BC1_RGB: shader mode 3 forces alpha 255 (view swizzle not needed). sRGB: shadow R8G8B8A8_SRGB view converts on sample. Block-texel-view / storage / copy image->buffer from BC (needs the compressed data; works, it is untouched) not done.
  6. Compressed image-to-image copies BC -> BC: copy compressed blocks and re-decode the destination.
- Hardware test queue: `t_bc_hw` (standalone Vulkan app, run with the driver ICD qdirect-style on CMF and Poco, `BC_DEV` selects the device): 12 formats x 2 mips of a 20x12 image, golden pixels
  from Mesa; PASS = max diff <= 1 (8-bit) / 2 half ulps, sRGB <= 2. Exit 77 = BC not advertised. Passes on Intel Xe2 (native sanity check of the golden: 0 failed). Pending: run on Mali after wiring.

(2026-11-17) CMF: all 12 BC formats PASS on hardware (t_bc_hw); see R4-PLAN.md R4-M7 status. Open: texelFetch with dynamic lod returns 0 in compute on CMF.

## R4-M0: PanVK oracle vs our driver (Poco, Mali-G925, vulkaninfo on the guest)
Oracle capture: `R4-ORACLE/vulkaninfo-panvk-poco.txt` (PanVK-kbase, Mesa 26.3.0-devel, apiVersion 1.4.363, 187 device extensions). Ours: apiVersion 1.0.363 (MALI_DEVICE_API_VERSION), 12 device extensions (swapchain, mutable_format, timeline_semaphore, external memory/fence/semaphore fd, dma_buf, drm_format_modifier, descriptor_update_template).
| item | ours | PanVK |
|---|---|---|
| maxImageDimension2D / 3D / ArrayLayers | 4096 / 256 / 256 | 65536 / 65536 / 1 (as reported by PanVK) |
| maxColorAttachments, maxBoundDescriptorSets | 4, 4 | 8, 7 |
| maxPushConstantsSize, maxComputeSharedMemory | 128, 16 KiB | 256, 32 KiB |
| maxDrawIndirectCount, maxMemoryAllocationCount | 1, 4096 | 4294967295 |
| timestampPeriod | 1 | 76.9231 ns |
| core features true | textureCompressionBC only | almost all: robustBufferAccess, fullDrawIndexUint32, imageCubeArray, independentBlend, geometry/tessellation, sampleRateShading, dualSrcBlend, logicOp, multiDrawIndirect, drawIndirectFirstInstance, depthClamp, depthBiasClamp, fillModeNonSolid, wideLines, largePoints, ETC2, occlusionQueryPrecise, pipelineStatisticsQuery, shaderInt64/Int16, samplerAnisotropy (not shaderFloat64) |
| format entries in the full dump | 12 | 38 |
Gaps that matter for dEQP, in order: (1) API version 1.0 vs 1.4 (no Vulkan 1.1-1.4 core, no dynamic rendering exposed, no sync2/maintenance*): CTS runs only the 1.0 subset; (2) feature bits all false: tests gate on them, so raising a bit means implementing it; (3) timestampPeriod 1 is not the real GPU timer period; (4) the format list is a small subset (render targets only RGBA8 until R4-M? work); (5) 1D/array/image limits are conservative. Our limits are deliberate (what is implemented), not a bug list.

## geometryShader design note (R4-B2, 2026-11-17; no implementation)
**Native hardware support: none.** Valhall/5th-gen job model is IDVS vertex -> tiler -> fragment (v10.xml has no GS stage; the "Geometry buffer" field at v10.xml:2051
is the IDVS position/varying heap, not a GS). Mesa panfrost (gallium and PanVK, tag 26.3.0) advertises no GS/tess; confirmed by the PanVK oracle dump.
**Emulation = compute prepass, as Honeykrisp/AGX does.** Mesa 26.3 now ships the reusable `src/poly` library (extracted from asahi): `poly_nir_lower_gs.c` (1504 lines:
rewrites the GS into a compute-style kernel that counts vertices, prefix-sums, writes the emitted vertices/indices into a heap, plus `poly_nir_passthrough_gs`,
`poly_nir_lower_vs`, restart/prim helpers) and `cl/geometry.cl` (377 lines of libagx OpenCL helpers, compiled to NIR with mesa_clc at build time). Plus `nir_lower_gs_intrinsics`, `nir_gs_count_vertices`.
Plan: (1) pipeline create: if a GS stage exists, compile VS to write its outputs to a buffer (`poly_nir_lower_vs`), GS to a compute kernel (count + emit passes), and the rest of the
pipeline as VS-less "indirect draw" fragment pipeline reading the generated vertex buffer (or a passthrough VS); (2) cmd draw: instead of one draw emit compute dispatch(VS) -> dispatch(GS count) ->
scan -> dispatch(GS emit) -> indirect indexed draw reading the heap, with a barrier chain; our existing expand-at-submit draw path (indirect draws are CPU-expanded, cc68908) can host the
indirect draw but needs GPU-written draw args => real indirect draw (GPU-side) is a prerequisite; (3) gl_PrimitiveID/gl_Layer/ViewportIndex passthrough, adjacency topologies, multiple streams (no: xfb is separate);
(4) needs `mesa_clc`/libclc in the cross build (not present in our build tree: new build dependency) or hand-written NIR for the count/emit kernels.
**Prereqs not yet in the driver**: GPU-written indirect args, device-side heap allocation + overflow handling, compute dispatch inside a render pass (needs pass split: compute between VS prepass and tiler job), writing VS outputs in compute (needs full VS interface lowering independent of the IDVS path).
**Effort**: 10-14 work-units (1 unit ~ one batch-2 feature): 3 libclc/poly build integration, 3 indirect draw from GPU args, 3 VS->buffer prepass + pass splitting, 2-3 GS lowering glue and tests (points/lines/triangles/adjacency, restart), 1 dEQP `geometry.*` pass-through. Cheaper partial option (2 units):
advertise geometryShader only for DXVK startup and fail pipeline creation for GS pipelines (not conformant: dEQP `geometry.*` would fail, and DXVK would hit it only with D3D10/11 apps that use GS; many titles do not). Recommend: do NOT stub; schedule as R5 with `src/poly` reuse.

## Poco daily 2026-11-17 (HEAD 4babc2a + GPU-GAP note, ours ICD, qdeqp daily-sample 1381, chunk 300, case-timeout 45)
283 pass / 126 fail / 971 NotSupported / 0 crash / 1 other (69.0 % of supported), 48 s. Identical to the 36aa680 baseline (283/126/971/0): case-by-case diff of the TSV against `deqp/ours-daily-sample-poco.tsv` = 0 changed cases, so 0 new failures and 0 newly passing. Raw: runs/qdeqp/poco-ours-daily-sample-hfjob.{tsv,csv}.

## CMF daily 2026-11-17 (Mali-G615 MC2 / v11, ours ICD from vk/out/a64-glibc built from a tree with the uncommitted head-flush edit, qdeqp daily-sample 1381, chunk 300)
282 pass / 127 fail / 971 NotSupported / 0 crash / 1 other (ResourceError) (68.8 % of supported), 49 s. vs Poco (283/126): 1 case differs, `api.image_clearing.core.clear_color_attachment.single_layer.a2r10g10b10_unorm_pack32_64x11_clamp_input_sample_count_4` (Fail on CMF, Pass on Poco; MSAA x4 clear, probably flaky/v11-specific, not triaged). The other 126 fails + 1 other are the same names as the Poco list. Raw: `deqp/ours-daily-sample-cmf.{tsv,csv}`.
Harness fix: `qdeqp.py` OKFOCUS did not know the CMF launcher (`com.nothing.launcher`) and waited 10 min; added.
Failing groups by likely cause (counts are CMF fails):
- **Stencil op combos** (pipeline.monolithic.stencil, 21: all `states.fail_*.pass_*.dfail_*` d32s8/d24s8/s8, comp_less/greater, incl. separate_layouts and nocolor): stencil op state (decrement-wrap/invert/replace combos with depth fail) wrong.
- **Blend** (pipeline.monolithic.blend, 10: 4-stage min/max/sub/add chains with 1-minus-dst-alpha, cc/ca constant factors, all formats): blend equation/factor encoding (MIN/MAX with factors, constant alpha).
- **Format reinterpret in blit/copy** (api.copy_and_blit.core, 23: blit_image and image_to_image with ETC2/EAC/BC on one side, e5b9g9r9 dst, 3d blit r4g4b4a4/r8_srgb general_general): compressed formats unsupported/unwired (BC decode is R4-M7), 3D blit.
- **Clear attachment** (api.image_clearing.core, 12: clear_color_attachment single/multiple layers, partial, d16 depth/stencil clear, sample_count_4): vkCmdClearAttachments via meta path (layers, partial rects, MSAA, d16).
- **Render-pass input attachments / sub-allocation** (renderpass.suballocation.formats 23 + attachment.1.13 + depth_stencil_write_conditions 2): input attachment reads (`input.*`, use_input_aspect, self_dep) and d16/d24 clear/load; loadOp/storeOp dont_care combos.
- **Sampler** (pipeline.monolithic.sampler, 9: border_swizzle gather_0 with riba/rgbi/igba on r8g8/r16g16/r32/d32; address_modes clamp_to_border on 2d_array/3d; 1d_array lod.equal_min_3_max_3): border colour + component swizzle (opaque_black) and gather, border/clamp modes, lod clamp.
- **render_to_image with combined depth/stencil formats** (7: r32_uint_s8_uint, a1r5g5b5_s8_uint, 1d/3d/cube_array mipmap): unsupported/unemulated D/S format pairs on array/mip layers.
- **api.info.image_format_properties** (3: 1d.linear a8b8g8r8_uint/a2r10g10b10_sint, 3d.linear a2b10g10r10_sint): linear-tiling capability limits reported wrongly.
- **Misc**: dynamic_offset (2), spec_constant.compute.composite.array.float, vertex_input (1), private_data.buffer_view_storage_r8g8b8a8_unorm, buffer_view.access (1), descriptor_set_layout_lifetime.compute (Other:ResourceError).

## texelFetch with push-constant lod returns zeros (CMF, investigation 2026-11-17: OFFLINE ONLY, hardware run blocked)
Hardware verification NOT done: the CMF dropped off USB (adb lists only the wireless entry, `qpreflight cmf` = "not listed") right after the probe was rebuilt; nothing below is confirmed on the device.
Added (no vk/src change): `shader/bc_probe.comp` kind 13 = `texelFetch(tex, ivec2, int(p.level))` (dynamic lod; regenerated `tests/t_bc_probe_spv.h`), `tests/t_txf_cc.c` + `tests/build-t_txf_cc.sh` (offline `mali_compile` of a probe SPIR-V; `BIFROST_MESA_DEBUG=shaders out/native/t_txf_cc 11 x.spv` dumps the Valhall disassembly + promoted immediates).
Run on the CMF when it is back: `BC_ICD=<bionic libvulkan_mali.so> BC_RGBA=1 BC_KIND=13 BC_DUMP=1 ./t_bc_hw` (bionic build: `aarch64-linux-android29-clang -include tests/t_bc_hw_direct.h tests/t_bc_hw.c -ldl -lm`), compare with BC_KIND=11 (constant lod).
Finding (disassembly, v11): the compiled code is correct in both cases. Constant lod 0 -> `TEX_FETCH.2d @r0:r1:r2:r3, @r2:r3, [handle:0]` (lod folded into the narrow descriptor word, no extra SR). Dynamic lod -> `min(level,255)`, `MKVEC.v2i8` x2 packs <0,0,0,lod> into one register and the fetch becomes `TEX_FETCH.2d.texel_offset @r0..r3, @r4:r5:r6, [handle:0]` (x, y, offset+lod word). Handle word and promoted immediates are identical to the constant case (0x0800003f = dummy sampler/texture handle), so the shader-side data is right. The only structural difference is the wide `texel_offset` source register (`pan_nir_lower_tex.c:938-960`, `va_collect_tex_srcs`: v11+ puts a *constant* txf lod in the narrow word `narrow_offset |= lod << 24`; a non-constant lod forces `is_narrow = false` and the SR u8vec4 path).
Root cause (hypothesis, ranked, unverified): (1) the texel_offset SR path for TEX_FETCH misbehaves on v11 (the SR layout/lod placement differs from the v9/v10 layout it was written for); constant lod, which avoids the SR, works; textureLod (TEX, explicit lod, no texel_offset) works. (2) The dummy sampler is an all-zero descriptor (lod clamp [0,0] + zero fields); fetch ignores it for constant lod but the explicit-lod SR path may consult it.
Proposed fix (to test first; patch in the vendored Mesa `third_party/mesa/src/panfrost/compiler/pan_nir_lower_tex.c`, NOT applied): keep a dynamic txf lod in the narrow word instead of the SR on arch >= 11:
```
-      if (tex->op == nir_texop_txf && srcs.lod) {
-         comps[3] = nir_get_scalar(srcs.lod, 0);
-         if (arch >= 11 && nir_scalar_is_const(comps[3])) {
+      nir_def *dyn_lod_narrow = NULL;
+      if (tex->op == nir_texop_txf && srcs.lod) {
+         comps[3] = nir_get_scalar(srcs.lod, 0);
+         if (arch >= 11 && narrow != NULL && !nir_scalar_is_const(comps[3])) {
+            dyn_lod_narrow = nir_ishl_imm(b, nir_umin_imm(b, comps[3].def, UINT8_MAX), 24);
+            comps[3].def = NULL;                    /* lod no longer in the SR offset */
+         } else if (arch >= 11 && nir_scalar_is_const(comps[3])) {
 ...  (and when building tex_h in va_lower_tex:  tex_h = nir_vec2(b, tex_h, dyn ? nir_ior_imm(b, dyn, narrow) : nir_imm_int(b, narrow));
```
(`narrow` must become a nir_def or carry the dynamic part through `va_collect_tex_srcs`; ~10 lines.) If hardware shows (2) instead, the fix is in the driver: fill the dummy sampler entry (`MALI_DUMMY_SAMPLER_IDX`, mali_descriptor.c / mali_sampler.c) with a valid default sampler (nearest, lod clamp [0,15], normalised/unnormalised coords as the other samplers) rather than zeros.
