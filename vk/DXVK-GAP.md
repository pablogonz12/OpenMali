# DXVK gap (ours vs what DXVK needs), 2026-11-17

(a) = DXVK refuses to create a device without it, (b) = common game paths, (c) = optional. Effort S/M/L. "HW" = does Mali + kbase already do it
(PanVK on the same GPU is the oracle). Source of truth for what is advertised: `t_dxvk_caps` + `mali_physical_device.c`.

| item | class | effort | HW / state |
|---|---|---|---|
| VK_KHR_swapchain, VK_EXT_robustness2 (nullDescriptor) | a | - | done |
| VK_EXT_depth_clip_enable | a | S | done: ZSD clip bit was already driven by the runtime state; test `t_vk_draw fx_dclip_*` (pixels, hardware) |
| VK_KHR_load_store_op_none | a | S | done: NONE load = LOAD path (colour preload); depth/stencil LOAD still unsupported; colour is always stored |
| VK_KHR_maintenance6 | a | S | done: BindDescriptorSets2/PushConstants2 forward to 1.0 paths; no descriptor-buffer parts |
| VK_KHR_maintenance5 | a | M | HW yes. Needs module-less stages (mali_pipeline.c:80), CmdBindIndexBuffer2, vkGetRenderingAreaGranularity, vkGetDeviceImageSubresourceLayout, VK_FORMAT_A1B5G5R5/A8_UNORM, default point size |
| VK_EXT_vertex_attribute_divisor | a | M | HW has per-attribute divisor in the attribute descriptor (PanVK does); we reject divisor > 1 (mali_pipeline.c:176) |
| VK_EXT_custom_border_color | a | M | HW: sampler border colour in the descriptor; sampler path in mali_sampler.c |
| VK_KHR_calibrated_timestamps | a | S-M | kbase has the CPU/GPU timestamp query (`ts_hz` known); needs a backend hook |
| VK_EXT_memory_budget | a | S | one heap, so budget = heap size minus tracked allocations |
| VK_EXT_extended_dynamic_state3 | a | M | runtime tracks the state; each dynamic state must be honoured in mali_cmd.c snapshot (sample mask, blend, polygon mode, ...) |
| VK_KHR_pipeline_library + VK_EXT_graphics_pipeline_library | a (3.x) | L | needs pipeline split compile + link; optional in DXVK 2.7 |
| core: robustBufferAccess, sampleRateShading, shaderCullDistance | a | M each | HW/compiler can; robustBufferAccess needs bounds-checked lowering |
| core: dualSrcBlend | a | M | HW has dual-source blend (second FS output); needs 2nd RT in FBD + blend lowering |
| core: geometryShader | a | L | Valhall has no GS hardware: PanVK does not offer it; needs compiler-side emulation |
| core: descriptorIndexing (full) | a | L | needs update-after-bind for all types (only sampled images/samplers today) |
| Sample-count 2/8, shaderStorageImageMultisample, tessellation | b/c | L | not planned |
