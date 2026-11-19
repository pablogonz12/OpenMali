# Feature/limit audit (feat-audit, 2026-11-18)
Static read of `vk/src/mali_physical_device.c` against `mali_cmd.c`/`mali_pipeline.c` unsupported() paths and the tests that probe each bit.
Honest (probed): timelineSemaphore, hostQueryReset, synchronization2, dynamicRendering, imagelessFramebuffer, maintenance4/5, privateData, BDA (no capture/replay),
scalar/ubo std layout, shaderDrawParameters, clip distance, depthClamp/depthClipEnable, rectangularLines only, custom border colour, inline uniform blocks,
demote, zero-init workgroup memory, subgroupSizeControl/computeFullSubgroups, sampled-image descriptor indexing subset, ycbcr (NV12 only).

Decisions: MSAA counts stay advertised (dEQP relies on them), maxColorAttachments stays 4 (the minimum), driverID stays MESA_PANVK (zink workarounds). See `vk/README.md` "Known gaps".

## Vulkan 1.3 mandatory features we do NOT set (best effort from the spec feature table, not verified line by line)
| feature | state | cost to enable |
|---|---|---|
| shaderTerminateInvocation | false | cheap in principle: NIR already has discard; needs a t_vk_feat pixel test (OpTerminateInvocation frag) on hardware |
| pipelineCreationCacheControl | false | cheap: no pipeline cache, so FAIL_ON_PIPELINE_COMPILE_REQUIRED would always return COMPILE_REQUIRED; the EARLY_RETURN_ON_FAILURE flag is honoured by both batch loops in `mali_pipeline.c` (later pipelines are not created after a failure), in both the `ci.flags` and the maintenance5 `VkPipelineCreateFlags2CreateInfo` form, so keep that. Touches mali_pipeline.c (hot) |
| robustImageAccess | false | medium: out-of-bounds image reads must return 0 (descriptor/tex lowering) |
| shaderIntegerDotProduct | false | medium: compiler support for OpSDot/UDot (lower to mul+add is legal, needs property bits too) |
| descriptorBindingInlineUniformBlockUpdateAfterBind | false | CONDITIONAL, not unconditionally mandatory: required only when `VK_EXT_descriptor_indexing`/`descriptorIndexing` is supported (neither is set today, see the 1.2 list). medium: UAB of inline blocks |
| textureCompressionASTC_HDR | false | not mandatory (needs one of BC/ETC2/ASTC_LDR; BC is set) |
## Vulkan 1.2 items to check (spec "required if 1.2"; status split, best effort from the feature table)
- Conditional (required only together with an extension/feature we do not set): drawIndirectCount (only if `VK_KHR_draw_indirect_count` is supported; maxDrawIndirectCount=1 now), the descriptorIndexing sub-features (only if `descriptorIndexing`/`VK_EXT_descriptor_indexing` is supported).
- Unconditional for a 1.2 device: shaderSubgroupExtendedTypes, separateDepthStencilLayouts, subgroupBroadcastDynamicId.
- Not classified against the spec yet: samplerFilterMinmax, shaderOutputViewportIndex/Layer (check the feature table before enabling).
**Umbrella inconsistency (known, not fixed here):** `descriptorIndexing` itself stays false while its
sampled-image sub-features are true, although the Vulkan 1.2 features table requires the umbrella bit
whenever one of the sub-features is enabled, and DXVK requires `descriptorIndexing` for its descriptor
model. This is a one-line honesty fix plus a `t_vk_feat` assertion; it is deliberately not part of this
change because it needs a `t_vk_feat didx` re-run and a golden re-bless. It is not required for FL11_0
games that never use descriptor indexing (Megabonk is one: D3D11 shader model 5.0 without bindless), so
it does not block a DXVK device for this title.
Set today: runtimeDescriptorArray, descriptorBindingPartiallyBound, descriptorBindingSampledImageUpdateAfterBind, descriptorBindingUpdateUnusedWhilePending, shaderSampledImageArrayDynamicIndexing/NonUniformIndexing (sampled-image subset, see the summary). All other descriptorIndexing sub-features and the rest of this list are false; costs unknown (hardware/compiler work).
