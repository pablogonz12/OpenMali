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
| pipelineCreationCacheControl | false | cheap: no pipeline cache, so FAIL_ON_PIPELINE_COMPILE_REQUIRED would always return COMPILE_REQUIRED; EARLY_RETURN flag is a no-op. Touches mali_pipeline.c (hot) |
| robustImageAccess | false | medium: out-of-bounds image reads must return 0 (descriptor/tex lowering) |
| shaderIntegerDotProduct | false | medium: compiler support for OpSDot/UDot (lower to mul+add is legal, needs property bits too) |
| descriptorBindingInlineUniformBlockUpdateAfterBind | false | medium: UAB of inline blocks |
| textureCompressionASTC_HDR | false | not mandatory (needs one of BC/ETC2/ASTC_LDR; BC is set) |
## Vulkan 1.2 items to check (spec "required if 1.2")
drawIndirectCount (maxDrawIndirectCount=1 now), descriptorIndexing as a whole, samplerFilterMinmax, shaderOutputViewportIndex/Layer, shaderSubgroupExtendedTypes, separateDepthStencilLayouts, subgroupBroadcastDynamicId. All false; costs unknown (hardware/compiler work).
