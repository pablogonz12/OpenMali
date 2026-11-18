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
| specialization constants (`pSpecializationInfo`, all stages) | hard in effect (composite shader variants) | | | soft | **have** (round 3, c0809ec): entries go to `spirv_to_nir` and into the compile-cache key; `t_compile spec`, `t_vk_feat spec` (smoke set) | done |
| core.shaderStorageImageWriteWithoutFormat | hard (was: vkCreateDevice -8) | | soft | hard | **have** (round 2): the descriptor carries the texel format, Mesa's `pan_nir_lower_image` reads it there; `t_vk_feat storfmt_nf` stores unformatted into 39 formats (smoke set, both farm routes) | done |
| vk11.samplerYcbcrConversion | hard (immutable NV12 sampler in the composite layout, ycbcr variants of the compute shaders) | | | | **have, NV12 only** (round 2, see below); `t_compile ycbcr` (lowering) and `t_vk_feat ycbcr` (pixels, smoke set) | done |
| EXT_physical_device_drm | soft (warns only) | | | | missing on purpose: we have no DRM render node; PanVK on the same ROM reports `/dev/mali0` as the render node via BL_DRM_STANDIN. Advertise only together with real major/minor reporting | S |
| KHR_maintenance5 | **hard for zink** (see the zink section) | | soft | soft | **have** (round 4): module-less stages, `vkCmdBindIndexBuffer2KHR`; `t_vk_feat maint5` | done |
| EXT_vertex_attribute_divisor, EXT_custom_border_color | | | soft | soft | missing, see DXVK-GAP.md | M each |
| EXT_extended_dynamic_state 1/2/3 | | | soft | soft | missing; each dynamic state must be read in the `mali_cmd.c` snapshot | M (1,2) / M (3) |
| EXT_shader_stencil_export, EXT_index_type_uint8, KHR_incremental_present, instance EXT_swapchain_colorspace | | | soft | | missing | S each (uint8 index needs an index-conversion path in `mali_cmd.c`; stencil export has the preload shader already) |
| geometryShader, robustBufferAccess, full descriptorIndexing, GPL | | | | hard | missing, see DXVK-GAP.md | L |

## Result of the offline experiment (no phone)
```
MALI_BACKEND=fake MALI_FAKE_GPU=v11 VK_DRIVER_FILES=<out>/native/mali_icd.json gamescope --backend headless -W 640 -H 360 -- sleep 5
```
Host gamescope 3.17.0.
- Before this branch: `vkCreateDevice failed (VkResult: -8)`, plus "does not support foreign queues, disabling modifier support" and "doesn't support VK_EXT_physical_device_drm".
- After round 1: the foreign-queue warning is gone and the extension list is accepted; `vkCreateDevice` still failed with -8 because of two hard features.
- After round 2 (both features in): `vkCreateDevice` succeeds, `t_steam_caps --expect-missing 0` passes. See "Round 2 result".

## Round 2 result (headless gamescope 3.17.0, fake v11, 80 s)
- `vkCreateDevice` succeeds; gamescope selects the device, lists the DRM formats and starts Xwayland; its pipeline thread compiles the compute pipelines with no `vkCreateComputePipelines failed`. All ten composite shaders (BLIT, BLUR, BLUR_COND, BLUR_FIRST_PASS, RCAS, EASU, NIS, NIS_HDR_LINEAR, RGB_TO_NV12, SGSR) compile: **our compiler rejects none of them.** (11 disk-cache entries: see the next item for why not hundreds.)
- Remaining errors in the log (neither stops it):
  1. `vkGetPhysicalDeviceFormatProperties2 returned zero modifiers for DRM format 0x3231564E` (NV12): NV12 is deliberately not on the DRM-modifier list (two memory planes; our WSI/dma-buf export is single-plane LINEAR), so NV12 clients cannot be imported as dma-bufs. Plain NV12 sampling of images gamescope itself creates works. Needs: multi-plane modifier plane layout + import in `mali_image.c` (L).
  2. `wlserver: Failed to get DRM FD from renderer`: no DRM render node on the fake backend (and `VK_EXT_physical_device_drm` missing on purpose, see table).
- **Gap found in round 2, closed in round 3 (c0809ec): specialization constants were ignored.** `mali_compile.c` calls `spirv_to_nir(..., NULL spec, ...)`, and `mali_pipeline.c` never reads `pSpecializationInfo`. gamescope's composite shaders are driven by spec constants (`layerCount`, `ycbcrMask`, `debug`, blur layers, colourspace mask, output EOTF, ITM): every pipeline compiles to the same default variant (hence 11 cache entries, not one per variant). On hardware gamescope would composite with the SPIR-V default constants. Plan: `struct mali_spec { uint32_t id, offset, size; }` + data in `mali_compile_opts` (hashed into the cache key), forwarded to `spirv_to_nir` as `nir_spirv_specialization`; `mali_pipeline.c` fills it from `pSpecializationInfo` for vs/fs/cs (that file is forbidden to this branch: patch in `patches/`).
- ycbcr scope: NV12 only, 2D, one level/layer, even extent, SAMPLED|TRANSFER only, not exportable, no DISJOINT. Models 709/601, ranges full/narrow, chroma siting midpoint or cosited-even, linear chroma filter (`forceExplicitReconstruction` = false). Other multi-planar formats are not advertised.

## zink (GL on Vulkan, needed by Steam's GL/ANGLE clients), round 4

**Tonight's phone result (guest, Mesa 26.2.4, `MESA_LOADER_DRIVER_OVERRIDE=zink`):** `glx: failed to create drisw screen`, `failed to load driver: zink`, `glXChooseVisual failed`. Release builds of zink print the reason only through `debug_printf`, so the log is silent about it.

**Why zink refused (found in the Mesa source, same list in 26.2.4 and 26.3):** `zink_get_physical_device_info` (`zink_device_info.py`) hard-requires `VK_KHR_maintenance1`, `VK_KHR_maintenance5`, `VK_KHR_create_renderpass2`, `VK_KHR_dynamic_rendering`, `VK_KHR_descriptor_update_template`; each is accepted when core-promoted at the device's apiVersion. Our device is 1.3 and maintenance5 only became core in 1.4, so **`VK_KHR_maintenance5` was the first hard requirement we lacked** (`ZINK: VK_KHR_maintenance5 required!`). Verified on the host: gdb showed `vkEnumeratePhysicalDevices` called and `vkCreateDevice` never.

| zink requirement | kind | state | effort |
|---|---|---|---|
| maintenance1, create_renderpass2, dynamic_rendering, descriptor_update_template | hard (core-promoted) | have | - |
| robustness2.nullDescriptor, timeline semaphore, external_memory_fd | hard | have | - |
| **KHR_maintenance5** | hard | **have (round 4)** | done |
| `have_KHR_maintenance1` extension flag (zink checks the extension, not the version) | base (warns) | **have (round 4)**: name advertised, `vkTrimCommandPoolKHR` called in `t_steam_caps` | done |
| `logicOp` | base (warns) | missing; Mali blend descriptors have a logic-op mode | M |
| `EXT_custom_border_color` | base (warns) | **have (round 4)**: custom colour + without-format; the runtime fills `border_color_value`, the sampler descriptor words 4..7 are the same path as the fixed colours. `t_vk_feat cbc` (creation + draw on the fake backend; the border pixels are checked on hardware only, NOT yet run) | done (pixel check pending) |
| `EXT_line_rasterization` | base (warns) | missing; only part of the modes maps to hardware | M |
| `EXT_physical_device_drm` (render node major/minor) | needed with a DRM render fd: zink 26.2.x `choose_pdev` fails without it (`failed to choose pdev`; 26.3 falls back for a single device). On the phone drisw passes fd=-1 | missing on purpose (no render node) | S |
| X11 surface (`VK_KHR_xcb_surface`) | only for the kopper route (DRI3); the phone's drisw path needs none | host x86 build lacks it, a64 build has it | S (build flag) |
| not measured yet | VS-only pipelines (rasterizer discard, unsupported in `mali_pipeline.c`), geometry shaders (cap the GL version), `A8_UNORM_KHR`/`A1B5G5R5` formats | | |

`t_steam_caps` has this list; `check.sh` pins the count of missing items (`ZINK_MISSING`).

**Order:** (1) maintenance5 (done), (2) the `maintenance1` flag (done), (3) `logicOp`, `line_rasterization` (custom_border_color done) (each only with a test and only when the hardware path works), (4) `EXT_physical_device_drm` once there is a real render node, (5) then whatever the next GL context creation shows.

**Host result (fake v10, headless gamescope Xwayland, `LIBGL_KOPPER_DRI2=1` because host Xwayland has no DRI3):** with maintenance5 zink creates its screen (`vkCreateDevice` is reached) and prints the base-requirement warning for `logicOp`, `have_KHR_maintenance1`, `custom_border_color`, `line_rasterization`. `glxinfo` then segfaults in context creation: kopper calls `vkCreateXcbSurfaceKHR` through a NULL pointer (struct sType 1000005000) because the x86 host ICD has no X11 surface extension. That is a host-build limit, not a driver bug.

**NV12 modifiers (gamescope):** gamescope asked `vkGetPhysicalDeviceFormatProperties2` for DRM format `0x3231564E` (NV12) and got zero modifiers: NV12 is deliberately not on our list (two memory planes; our dma-buf export is single-plane LINEAR). It matters only when a client hands gamescope NV12 dma-bufs (video). Steam's UI (CEF/ANGLE) is RGB and does not need it, so it is non-fatal for the menu. The fix is a multi-plane modifier layout plus import in `mali_image.c` (L).

## Phone experiment (not run: phones busy)
After the two features are in and `t_steam_caps --expect-missing 0` passes, start the MediaDeck session on our ICD (`droiddeck-session`, lines ~202 and ~293):
```
BL_OPEN_DRIVER=1 BL_DRM_STANDIN=/dev/mali0 BL_VK_DRIVER=<guest path>/mali_icd.json droiddeck-session
```
and read the first `vulkan:` line of the gamescope log (`== vulkan driver: imported ...` confirms the ICD was picked up). The session script's `BL_VKBRIDGE_FALLBACK_ICD` path uses the PanVK pair and is the oracle. Do not use the bridge fallback for this test.
To exercise gamescope alone on hardware first, run it headless under the farm's guest with `VK_DRIVER_FILES=<icd>` as in the host experiment above.

## Needs in files this branch may not touch (other workers)
- `mali_cmd.c`: honouring the dynamic states of EXT_extended_dynamic_state 1/2/3; index conversion for `index_type_uint8`.
- `mali_cs_emit.c`: nothing is required for the Steam menu itself; dual-source blend and sample mask (ds3) land here later.
- `mali_pipeline.c`: pass `pSpecializationInfo` to `mali_compile` (gamescope needs it, see Round 2 result); vertex attribute divisor > 1 (line ~176), immutable ycbcr samplers when pipelines are built.
- BO allocator: tiled/AFBC modifier images need dma-buf export of non-linear BOs and a stride/modifier plane layout; LINEAR works today.

## Risks
- gamescope forces compositing on the bridge (`gamescope_composite_force=1`); on our driver it will take the compute composite path, so shader compile coverage matters more than WSI.
- Not testable offline: Wayland WSI, dma-buf import from clients, present timing, every shader result (the fake backend does not execute).
- Smoke on the farm runs only `t_vk_draw`, `t_vk_compute`, `t_vk_meta`: extension-table changes are checked on hardware only by device creation.
