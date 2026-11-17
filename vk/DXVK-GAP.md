# DXVK gap (ours vs what DXVK needs), 2026-11-17

(a) = DXVK refuses to create a device without it, (b) = common game paths, (c) = optional. Effort S/M/L. "HW" = does Mali + kbase already do it
(PanVK on the same GPU is the oracle). Source of truth for what is advertised: `t_dxvk_caps` + `mali_physical_device.c`.

| item | class | effort | HW / state |
|---|---|---|---|
| VK_KHR_swapchain, VK_EXT_robustness2 (nullDescriptor) | a | - | done |
| VK_EXT_depth_clip_enable | a | S | done: ZSD clip bit was already driven by the runtime state; test `t_vk_draw fx_dclip_*` (pixels, hardware) |
| VK_KHR_load_store_op_none | a | S | done: NONE load = LOAD path (preload draw for colour, depth and stencil); colour is always stored |
| VK_KHR_maintenance6 | a | S | done: BindDescriptorSets2/PushConstants2 forward to 1.0 paths; no descriptor-buffer parts |
| VK_KHR_maintenance5 | a | M | HW yes. Needs module-less stages (mali_pipeline.c:80), CmdBindIndexBuffer2, vkGetRenderingAreaGranularity, vkGetDeviceImageSubresourceLayout, VK_FORMAT_A1B5G5R5/A8_UNORM, default point size |
| depth/stencil loadOp LOAD (core, not an extension) | a+ | L | done on v11 hardware (Mali-G615, `t_vk_draw load_depth load_depth16 load_stencil`, farm 20261117-182858-25ecccaf); v13 pixels untested. Preload draw (`mali_meta_preload`, `shader/preload_zs.frag`) writes gl_FragDepth / stencil export, as PanVK does. D32_SFLOAT and D16_UNORM depth, stencil, single-sampled. Still rejected: MSAA depth/stencil LOAD, D24 depth LOAD (`mali_cmd.c` "depth loadOp LOAD ..."). What was wrong: `mali_compile.c` never recorded FRAG_RESULT_DEPTH/STENCIL in `outputs_written` before `pan_nir_lower_fs_outputs`, so `fs.writes_depth/stencil` were 0 for every shader: no ZSD depth source = Shader, no stencil-from-shader, no force-late ZS (`mali_cs_emit.c`). Same bug made any application gl_FragDepth shader wrong. Also needed: Z/S-only FS has no render target in the DCD (rt mask, write mask, blend count 0), otherwise the GPU hangs |
| VK_EXT_vertex_attribute_divisor | a | M | HW has per-attribute divisor in the attribute descriptor (PanVK does); we reject divisor > 1 (mali_pipeline.c:176) |
| VK_EXT_custom_border_color | a | M | HW: sampler border colour in the descriptor; sampler path in mali_sampler.c |
| VK_KHR_calibrated_timestamps | a | S | done: backend `gpu_timestamp` (GET_CPU_GPU_TIMEINFO) + runtime `vk_common_GetCalibratedTimestampsKHR`; `t_dxvk_caps` |
| VK_EXT_memory_budget | a | S | done: usage = live vkAllocateMemory bytes, budget = heap; `t_dxvk_caps` alloc/free check |
| VK_EXT_extended_dynamic_state3 | a | M | runtime tracks the state; each dynamic state must be honoured in mali_cmd.c snapshot (sample mask, blend, polygon mode, ...) |
| VK_KHR_pipeline_library + VK_EXT_graphics_pipeline_library | a (3.x) | L | needs pipeline split compile + link; optional in DXVK 2.7 |
| core: robustBufferAccess, sampleRateShading, shaderCullDistance | a | M each | HW/compiler can; robustBufferAccess needs bounds-checked lowering |
| core: dualSrcBlend | a | M | HW has dual-source blend (second FS output); needs 2nd RT in FBD + blend lowering |
| core: geometryShader | a | L | Valhall has no GS hardware: PanVK does not offer it; needs compiler-side emulation |
| core: descriptorIndexing (full) | a | L | needs update-after-bind for all types (only sampled images/samplers today) |
| Sample-count 2/8, shaderStorageImageMultisample, tessellation | b/c | L | not planned |
