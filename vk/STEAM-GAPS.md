# Steam-menu gap table (ours vs gamescope + Steam CEF/ANGLE + DXVK), 2026-11-18

Goal: the Steam menu inside the MediaDeck guest on our driver (today only the Mali bridge does this). Source of truth for what we advertise:
`t_steam_caps` (this item list, gated by `STEAM_MISSING` in check.sh), `t_dxvk_caps`, `mali_physical_device.c`, `mali_instance.c`.
Effort: S < 1 day, M 1-3 days, L > 3 days. "HW" = Mali can do it (PanVK is the oracle).

## What the session runs
- gamescope (Wayland backend in the guest). It does not use a Vulkan swapchain for output; it creates its own device, imports client images, composites with compute shaders.
- Steam CEF/ANGLE: `+Vulkan +VulkanFromANGLE +DefaultANGLEVulkan`. ANGLE needs Vulkan 1.1 and a graphics queue; extensions and features are optional and used when present.
- Steam X11 clients go through gamescope's WSI layer (`VkLayer_FROG_gamescope_wsi`), which forces `VK_KHR_wayland_surface`, `VK_KHR_xcb_surface` onto the instance and `VK_EXT_swapchain_maintenance1` onto the device, then uses the driver's own Wayland WSI (Mesa wsi_common).
- DXVK is for games, not for the menu; its gaps stay in `DXVK-GAP.md`.

## Gap table

| item | gamescope | WSI layer | ANGLE | DXVK | state | size |
|---|---|---|---|---|---|---|
| apiVersion >= 1.3, graphics+compute queue | hard | | hard (1.1) | hard | have | - |
| timelineSemaphore, dynamicRendering, scalarBlockLayout | hard | | | | have | - |
| robustness2.nullDescriptor | hard | | | hard | have | - |
| KHR_external_memory_fd, EXT_external_memory_dma_buf, KHR_external_semaphore_fd | hard | | soft | | have | - |
| EXT_image_drm_format_modifier | soft | | | | partial: LINEAR only (our "optimal" is linear) | M for tiled modifiers (AFBC/16x16), kbase import path |
| EXT_queue_family_foreign | soft (without it modifiers are switched off) | | | | have (this branch; ownership indices are ignored by the barrier code, `t_steam_caps` barriers) | S |
| KHR_present_id, KHR_present_wait (feature structs chained always) | hard | | | | have (this branch; wsi_common) | S |
| EXT/KHR_swapchain_maintenance1 | | hard (forced) | | | have (this branch; wsi_common) | S |
| instance EXT_surface_maintenance1 | | hard | | | have (this branch) | S |
| KHR_swapchain, swapchain_mutable_format, external_fence_fd | soft | hard | soft | hard | have | - |
| KHR_dynamic_rendering, synchronization2, maintenance4, EXT_private_data, KHR_buffer_device_address, EXT_scalar_block_layout, EXT_host_query_reset, KHR_uniform_buffer_standard_layout (names of promoted features) | | | soft (checks the name) | soft | have (this branch; feature bit true and probed, name==bit checked by `t_steam_caps`) | S |
| instance KHR_wayland_surface, KHR_xcb_surface | | hard | | | have in the a64-glibc-wl Mesa build; NOT in the x86 native build, so WSI cannot be tested on the host | S (host build flag) |
| **core.shaderStorageImageWriteWithoutFormat** | **hard: vkCreateDevice returns -8 without it** | | soft | hard | missing. Format feature bits already report `STORAGE_WRITE_WITHOUT_FORMAT`; the compiler path for `image_store` with `PIPE_FORMAT_NONE` (format taken from the descriptor) is untested: `t_vk_feat storfmt` shaders all carry an explicit format. Needs a lowering in `mali_descriptor_lower.c`/`mali_compile.c` (per-format store emulation or a texture-descriptor driven ST_CVT) and a hardware pixel test. Cannot be proven on the fake backend | M |
| **vk11.samplerYcbcrConversion** | **hard: gamescope always creates the immutable NV12 sampler in its composite layout, and precompiles the ycbcr variants of its compute shaders at init** | | | | missing. Needs `nir_vk_lower_ycbcr_tex`, immutable-sampler plumbing in `mali_pipeline_layout.c`/`mali_sampler.c`, multi-plane NV12 images (`mali_image.c`, `mali_format.c`), plane-wise texture descriptors | M-L |
| EXT_physical_device_drm | soft (warns only) | | | | missing on purpose: we have no DRM render node; PanVK on the same ROM reports `/dev/mali0` as the render node via BL_DRM_STANDIN. Advertise only together with real major/minor reporting | S |
| KHR_maintenance5, EXT_vertex_attribute_divisor, EXT_custom_border_color | | | soft | soft | missing, see DXVK-GAP.md | M each |
| EXT_extended_dynamic_state 1/2/3 | | | soft | soft | missing; each dynamic state must be read in the `mali_cmd.c` snapshot | M (1,2) / M (3) |
| EXT_shader_stencil_export, EXT_index_type_uint8, KHR_incremental_present, instance EXT_swapchain_colorspace | | | soft | | missing | S each (uint8 index needs an index-conversion path in `mali_cmd.c`; stencil export has the preload shader already) |
| geometryShader, robustBufferAccess, full descriptorIndexing, GPL | | | | hard | missing, see DXVK-GAP.md | L |

## Result of the offline experiment (no phone)
```
MALI_BACKEND=fake MALI_FAKE_GPU=v11 VK_DRIVER_FILES=<out>/native/mali_icd.json gamescope --backend headless -W 640 -H 360 -- sleep 5
```
Host gamescope 3.17.0.
- Before this branch: `vkCreateDevice failed (VkResult: -8)`, plus "does not support foreign queues, disabling modifier support" and "doesn't support VK_EXT_physical_device_drm".
- After this branch: the foreign-queue warning is gone and the extension list is accepted; `vkCreateDevice` still fails with -8 because of the two hard features above. Those are the first blockers, in this order:
  1. `shaderStorageImageWriteWithoutFormat`
  2. `samplerYcbcrConversion`
- Beyond them (untested because the device is not created): gamescope compiles its compute pipelines at start (composite, blur, ycbcr variants, `image_store` without format). Expect the compiler to be the next failure; the first one will show on a real device (below).

## Phone experiment (not run: phones busy)
After the two features are in and `t_steam_caps --expect-missing 0` passes, start the MediaDeck session on our ICD (`droiddeck-session`, lines ~202 and ~293):
```
BL_OPEN_DRIVER=1 BL_DRM_STANDIN=/dev/mali0 BL_VK_DRIVER=<guest path>/mali_icd.json droiddeck-session
```
and read the first `vulkan:` line of the gamescope log (`== vulkan driver: imported ...` confirms the ICD was picked up). The session script's `BL_VKBRIDGE_FALLBACK_ICD` path uses the PanVK pair and is the oracle. Do not use the bridge fallback for this test.
To exercise gamescope alone on hardware first, run it headless under the farm's guest with `VK_DRIVER_FILES=<icd>` as in the host experiment above.

## Needs in files this branch may not touch (other workers)
- `mali_cmd.c`: honouring the dynamic states of EXT_extended_dynamic_state 1/2/3; index conversion for `index_type_uint8`; `CmdBindIndexBuffer2` (maintenance5).
- `mali_cs_emit.c`: nothing is required for the Steam menu itself; dual-source blend and sample mask (ds3) land here later.
- `mali_pipeline.c`: module-less stages (maintenance5), vertex attribute divisor > 1 (line ~176), immutable ycbcr samplers when pipelines are built.
- BO allocator: tiled/AFBC modifier images need dma-buf export of non-linear BOs and a stride/modifier plane layout; LINEAR works today.

## Risks
- gamescope forces compositing on the bridge (`gamescope_composite_force=1`); on our driver it will take the compute composite path, so shader compile coverage matters more than WSI.
- Not testable offline: Wayland WSI, dma-buf import from clients, present timing, every shader result (the fake backend does not execute).
- Smoke on the farm runs only `t_vk_draw`, `t_vk_compute`, `t_vk_meta`: extension-table changes are checked on hardware only by device creation.
