> **Which driver is the guest really using?** Run `tools/inspect-session --serial <ip:port>` (add `--json`, or `--watch 5`). Read-only, no lock. It runs `droiddeckctl` (default `~/Projects/mali-gaming/tools/droiddeckctl`, override with `DROIDDECKCTL=<path>`); if that fails it says `state unavailable` (exit 1), not `no session`. It prints, per gamescope/Xwayland/steam/steamwebhelper/game process, the Vulkan ICD and GL libs mapped in the guest (verdict OURS / BRIDGE / SOFTWARE / NONE), the renderer string Steam's CEF reports over CDP, on-screen fps from SurfaceFlinger pacing, and GPU freq/busy when adb may read them (usually denied). "Mapped" is not "used", so read it together with the fps line.

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
| instance KHR_wayland_surface, KHR_xcb_surface, KHR_xlib_surface | | hard | | | **have** (round 6): Wayland + XCB + Xlib in the a64-glibc-wsi and host `build-vk-x86-x11` Mesa builds (`-Dplatforms=wayland,x11`), see "X11 WSI". A build without x11 advertises no xcb/xlib surface; `t_vk_xcb`, `xwsi-host.sh` (smoke set) | done |
| specialization constants (`pSpecializationInfo`, all stages) | hard in effect (composite shader variants) | | | soft | **have** (round 3, c0809ec): entries go to `spirv_to_nir` and into the compile-cache key; `t_compile spec`, `t_vk_feat spec` (smoke set) | done |
| core.shaderStorageImageWriteWithoutFormat | hard (was: vkCreateDevice -8) | | soft | hard | **have** (round 2): the descriptor carries the texel format, Mesa's `pan_nir_lower_image` reads it there; `t_vk_feat storfmt_nf` stores unformatted into 39 formats (smoke set, both farm routes) | done |
| vk11.samplerYcbcrConversion | hard (immutable NV12 sampler in the composite layout, ycbcr variants of the compute shaders) | | | | **have, NV12 only** (round 2, see below); `t_compile ycbcr` (lowering) and `t_vk_feat ycbcr` (pixels, smoke set) | done |
| EXT_physical_device_drm | soft (warns only) | | | | missing on purpose: we have no DRM render node; PanVK on the same ROM reports `/dev/mali0` as the render node via BL_DRM_STANDIN. Advertise only together with real major/minor reporting | S |
| KHR_maintenance5 | **hard for zink** (see the zink section) | | soft | soft | **have** (round 4): module-less stages, `vkCmdBindIndexBuffer2KHR`; `t_vk_feat maint5` | done |
| EXT_vertex_attribute_divisor, EXT_custom_border_color | | | soft | soft | **have**: custom border colour (`t_vk_feat cbc`), divisor with `vertexAttributeInstanceRateDivisor` = false (only divisor 0/1; `t_vk_feat divisor`) | done |
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
| `logicOp` | base (warns) | missing, NOT advertised: `mali_cmd.c:495` marks logic-op draws unsupported; needs the blend descriptor's logic-op (or a fragment-shader lowering) in `mali_cmd.c`/`mali_cs_emit.c`/`mali_pipeline.c` (other worker's files): note in `patches/zink-base-requirements.NOTE.md` | M |
| `EXT_custom_border_color` | base (warns) | **have (round 4)**: custom colour + without-format; the runtime fills `border_color_value`, the sampler descriptor words 4..7 are the same path as the fixed colours. `t_vk_feat cbc` (creation + draw on the fake backend; the border pixels are checked on hardware only, NOT yet run) | done (pixel check pending) |
| `EXT_line_rasterization` | base (warns) | **advertised, `rectangularLines` only** (`mali_physical_device.c`, `t_vk_feat linerast`): the draw path uses the hardware's default lines, width from `lineWidth`. Stippled/Bresenham/smooth stay false (no hardware path, zink falls back to its own emulation) | done (rectangular) |
| `EXT_physical_device_drm` (render node major/minor) | needed with a DRM render fd: zink 26.2.x `choose_pdev` fails without it (`failed to choose pdev`; 26.3 falls back for a single device). On the phone drisw passes fd=-1 | missing on purpose (no render node) | S |
| X11 surface (`VK_KHR_xcb_surface`) | only for the kopper route (DRI3); the phone's drisw path needs none | host x86 build lacks it, a64 build has it | S (build flag) |
| not measured yet | VS-only pipelines (rasterizer discard, unsupported in `mali_pipeline.c`), geometry shaders (cap the GL version), `A8_UNORM_KHR`/`A1B5G5R5` formats | | |

`t_steam_caps` has this list; `check.sh` pins the count of missing items (`ZINK_MISSING`).

**Order:** (1) maintenance5 (done), (2) the `maintenance1` flag (done), (3) `logicOp`, `line_rasterization` (custom_border_color done) (each only with a test and only when the hardware path works), (4) `EXT_physical_device_drm` once there is a real render node, (5) then whatever the next GL context creation shows.

**Host result, round 4 (fake v10, host Mesa 26.2.6 zink):**
1. kopper/GLX through headless gamescope's Xwayland (`LIBGL_KOPPER_DRI2=1`, host Xwayland has no DRI3): screen created, then a NULL call in `vkCreateXcbSurfaceKHR` (sType 1000005000): the x86 host ICD has no X11 surface extension (the a64 build has it; the phone's drisw path needs no surface). Host-build limit.
2. EGL surfaceless / GBM platforms (DRM render fd): `ZINK: failed to choose pdev`, falls back to llvmpipe: 26.2.x needs `VK_EXT_physical_device_drm` renderMajor/Minor to match the fd; we have no render node, so it stays unadvertised (S once a node exists; 26.3 has a single-device fallback).
3. **EGL device platform (no fd, `tests/zink-host.sh`, `check.sh` "zink host triangle"): a GL context is created on our device** (`zink Vulkan 1.3(Mali-G610 MC6 ...)`, compat 2.1 / GLES 2.0 only: no core profile yet, reason not measured), shaders compile, one triangle is drawn and `glReadPixels` returns `GL_NO_ERROR` (pixels are real only on hardware). The base-requirement warning now names only `logicOp` and `EXT_line_rasterization`.
4. Its first crash was a gap in our driver, now fixed: zink always uses the core 1.3 `vkCmdBindVertexBuffers2`, which this runtime routes to `vkCmdBindVertexBuffers3KHR`, which we did not implement (NULL call at the first draw). `mali_vb3.c`; `t_vk_draw` rebinds through the 2-form.
5. **Dynamic vertex binding stride: fixed** (`ade632c`, kept across plain binds in `ba4dc6a`). `pBindingInfos[].setStride` is stored per binding (`cmd->vb_stride`, `mali_vb3.c`) and replaces the pipeline stride when it declares `VERTEX_INPUT_BINDING_STRIDE` dynamic (`gfx.dyn_stride`, `mali_cs_emit.c`); a stride change rebuilds the shared driver set. A plain `vkCmdBindVertexBuffers` after the 2-form and a 2-form bind without `setStride` keep the stride. Smoke `t_vk_draw` cases `dyn_stride` and `dyn_stride_rebind` (the latter follows the 2-form bind with a plain bind; pixels checked on hardware only, skipped on the fake backend). Still unmeasured: other extended-dynamic-state setters honoured by `mali_cmd.c`.

**NV12 modifiers (gamescope):** gamescope asked `vkGetPhysicalDeviceFormatProperties2` for DRM format `0x3231564E` (NV12) and got zero modifiers: NV12 is deliberately not on our list (two memory planes; our dma-buf export is single-plane LINEAR). It matters only when a client hands gamescope NV12 dma-bufs (video). Steam's UI (CEF/ANGLE) is RGB and does not need it, so it is non-fatal for the menu. The fix is a multi-plane modifier layout plus import in `mali_image.c` (L).

## Why zink offers only compat 2.1 / GLES 2.0 (round 5, source analysis; host Mesa 26.2.6 and the checked-out Mesa)

Mesa's GL version is computed in `src/mesa/main/version.c` (`compute_version`, `compute_version_es2`) from the extension flags that `st_extensions.c` derives from zink's caps (`zink_screen.c`, `zink_init_screen_caps`). Checked against our device (host fake v10, `vulkaninfo`, `--show-formats`):

| Level | What decides it | Ours | Blocker | Effort |
|---|---|---|---|---|
| GL 2.1 / ES 2.0 | shaders, blend separate, two-sided stencil, sRGB | have | - | what we get today |
| **GL 3.0** | `EXT_transform_feedback` (`EXT_CAP(EXT_transform_feedback, max_stream_output_buffers)`); zink sets `max_stream_output_buffers` only when `VK_EXT_transform_feedback` (feature `transformFeedback`) is present | **missing** | **`VK_EXT_transform_feedback`** | **L**: Valhall has no hardware stream-out; it needs a vertex-shader lowering to buffer stores (xfb info through NIR), begin/end + counter buffers, and VS-only draws (below); compiler and draw-path files |
| **ES 3.0** | the same `EXT_transform_feedback`, plus `ARB_instanced_arrays` (zink: `vertex_element_instance_divisor` = `VK_EXT_vertex_attribute_divisor`) | both **missing** | `VK_EXT_transform_feedback` (L), `VK_EXT_vertex_attribute_divisor` (M: instance divisor in the attribute-buffer descriptor emitted by `mali_cs_emit.c`/`mali_pipeline.c`, draw path) | L + M |
| GL 3.1 / 3.2 | UBO (`maxUniformBufferRange` 16384 ok), 16 vertex texture units (our 16 per-stage samplers ok), depth clamp (`depthClamp` + `VK_EXT_depth_clip_enable`: have), provoking vertex (zink flags it from `VK_EXT_provoking_vertex`, which we lack; not measured whether zink then falls back), sync, multisample (ok) | mostly have | provoking vertex unmeasured | M (draw path) if needed |
| GL 3.3 | `ARB_instanced_arrays` (same divisor extension), dual-source blend (have), timer query (`timestampValidBits` = 64: have), swizzle (have) | divisor missing | `VK_EXT_vertex_attribute_divisor` | M |
| GLES 3.1 | compute (have), `maxVertexInputBindingStride` >= 2048 (have, exactly 2048), draw indirect (have), image load/store, SSBO | have in principle | needs ES 3.0 first | - |
| GLES 3.2 / GL 4.x | geometry + tessellation shaders (`geometryShader`, `tessellationShader` false) | missing | stages absent from the compiler | L each |

**Formats are not the blocker:** every format Mesa's 3.0 checks name is listed by our device (R11G11B10, RGB9E5, D32F, SNORM 8/16, BC4/BC5, A2B10G10R10 UNORM/UINT, sRGB 8888, float16/32, integer formats, ETC2/EAC/ASTC, 4444/565). Missing: `A8_UNORM_KHR` (maintenance5 format, used by zink for GL_ALPHA8; S-M, descriptor swizzle, needs a pixel test on hardware).

**Counts:** blockers for GL 3.0: 1 (L); for ES 3.0: 2 (1 L + 1 M); extra for GL 3.3: 0 beyond the divisor; unmeasured: 1 (provoking vertex). The draw-path prerequisites (rasterizer discard / VS-only pipelines, `mali_cmd.c:426`) are an M item that XFB needs in practice (GL_RASTERIZER_DISCARD).

**Implemented this round: none.** No S item in this list can be advertised honestly: every blocker touches the compiler or the draw-path files, and Steam's web helper (ANGLE/GL ES 3) therefore stays at GLES 2.0 on zink until `VK_EXT_transform_feedback` and the divisor exist. Order: divisor (M, also needed by DXVK, DXVK-GAP.md) -> VS-only pipelines -> transform feedback (L).

**Phone result (round 5, poco-x4, a64-glibc-wl build of aa760ff, Mesa 26.2.4 in the guest):** the ICD loads and gamescope selects it; `vulkaninfo` lists the device. zink still prints `glx: failed to create drisw screen` / `failed to load driver: zink` (glxinfo and Steam's web helper, EGL `failed to create dri2 screen`) with no `MESA: error` line and no GL version: the screen creation fails silently before any context exists, so the GL level above is predicted from the source, not measured on the phone. Steam dies 11 s into the session (`GUEST_EXIT`, no frame). Next measurement: run `glxinfo` in the guest with `VK_LOADER_DEBUG=all` to see whether `vkCreateDevice` is reached (the host run reaches it).

## X11 WSI (round 6)

**Question:** is the silent `glx: failed to create drisw screen` caused by the missing X11 WSI? **Answer: no, refuted for the screen failure; confirmed for the step after it.** Hypothesis check on the host (headless gamescope's Xwayland, `glxinfo -B`, `MESA_LOADER_DRIVER_OVERRIDE=zink`, fake backend, same Mesa 26.2.6 zink as the guest's 26.2.4):
- Screen creation (`zink_internal_create_screen`) never touches a surface. With the old ICD (no X11 platform) the screen is created; `glxinfo` then dies in the first drawable (the old native build has a NULL `vkCreateXcbSurfaceKHR`; with the gamescope layer on it reaches `zink: could not create swapchain`). So the host does **not** print `failed to create drisw screen`; it prints `could not create swapchain`, a different, later and *loud* failure.
- The phone's silent screen failure is therefore something in the screen path. Silent exits in release builds (`debug_printf` is compiled out): `!robustness2.nullDescriptor` (no message at all), `zink_get_physical_device_info` false (a missing required device extension, `debug_printf` only), `check_have_device_time`, `zink_create_logical_device`. The `maintenance5` fix (4be52fa) did not clear it on the phone, so one of these still fails on the real backend. **Not measured yet**; next phone run: `VK_LOADER_DEBUG=all MESA_DEBUG=1 ZINK_DEBUG=` with the `a64-glibc-wsi` library, and check `NV_linear_color_attachment` (the hard-required list in 26.2/26.3: maintenance1/4/5, create_renderpass2, dynamic_rendering, descriptor_update_template, workgroup_memory_explicit_layout, pipeline_executable_properties, external_semaphore_fd, NV_linear_color_attachment, push_descriptor).

**Round 6b: the screen failure, measured (phone, READY bridge session, our ICD selected only for the test processes) and diagnosed (host).**
- The guest's Mesa is **26.0.8** (`26.0.8-1ubuntu0.3+droiddeck1`), not 26.2.x. The guest has `/dev/mali0` and `/dev/dri/renderD113`. Its Xwayland (`:0`, `:1`) offers **DRI3, Present, MIT-SHM, GLX, XFIXES, SYNC, RANDR, XWAYLAND** (the "no glamor, no DRI3" remark below describes the sources of an older build and is wrong for this guest).
- `glxinfo` with `MESA_LOADER_DRIVER_OVERRIDE=zink`: `glx: failed to create drisw screen` / `failed to load driver: zink`, no `MESA: error` line. The loader log shows our ICD loaded and the physical device enumerated, then the instance destroyed: **vkCreateDevice is never called**, so the failure sits between enumeration and device creation, in `zink_get_physical_device_info`.
- Cause (Mesa 26.0.8 `zink_device_info.py`, read from the 26.0.8 tag): `VK_KHR_maintenance1`, **`VK_KHR_create_renderpass2`**, **`VK_KHR_imageless_framebuffer`**, `VK_KHR_dynamic_rendering`, `VK_KHR_descriptor_update_template` are `required=True` and matched **by extension name** (`strcmp` on the device's extension list; the api version does not stand in). A miss is `debug_printf("ZINK: X required!")` (compiled out of release builds) and `goto fail`, so the screen fails without a word. Our ICD listed the first, fourth and fifth, but **not renderpass2 and imageless_framebuffer**. The earlier t_steam_caps check accepted "core 1.2" for renderpass2 and so could not see it: it now checks the five names. Fixed in 97d05d7: both names advertised (+ `imagelessFramebuffer`), both are runtime behaviour (`vk_common_CreateRenderPass2`, `CmdBeginRenderPass2` with `VkRenderPassAttachmentBeginInfo`); t_vk_feat `imageless` renders a clear through RenderPass2 + an imageless framebuffer (pixels on hardware).
- `eglinfo` (GBM/device platform) prints `MESA: error: ZINK: failed to choose pdev`: 26.0.8's `choose_pdev` has no single-device fallback and the render-node match needs `VK_EXT_physical_device_drm`, which stays unadvertised (needs a DRM node identity the driver does not own: `renderD113` is the bridge's, not ours). GLX/kopper does not take this path.
- `tests/x11guest.sh` on the phone: `t_vk_xcb` with `MALI_WSI=sw --pixels` PASS (4 images 1638x720, 3 frames, window pixel BGR 204,102,51 as expected). `MALI_WSI=dmabuf` fails in `CreateSwapchainKHR` (informational). The default run passes too.

**What kopper (zink's GLX window system) asks for** (`zink_kopper.c`, `zink_instance.py`, `zink_device_info.py`, Mesa 26.2/26.3): instance `VK_KHR_surface`, `VK_KHR_xcb_surface` (also wayland/win32 surface, `EXT_swapchain_colorspace` optional); device `VK_KHR_swapchain` (`swapchain_mutable_format` optional); entrypoints `CreateXcbSurfaceKHR`, `DestroySurfaceKHR`, `GetPhysicalDeviceSurfaceSupportKHR`, `GetPhysicalDeviceSurfaceCapabilitiesKHR`, `GetPhysicalDeviceSurfacePresentModesKHR` (+ Formats), `CreateSwapchainKHR`, `GetSwapchainImagesKHR`, `AcquireNextImageKHR`, `QueuePresentKHR`, `DestroySwapchainKHR`. What we lacked before this round: **`VK_KHR_xcb_surface` and its entrypoints** only (the other items come with wsi_common). Without `LIBGL_KOPPER_DISABLE` zink always goes to kopper for GLX; Steam's `steamwebhelper.sh` sets `LIBGL_KOPPER_DISABLE` and DroidDeck's preload unsets it again, because without kopper (and without DRI3) zink gets no GL context.

**What the driver advertised for surfaces, and what gamescope uses instead**

| build | instance surface extensions before | now |
|---|---|---|
| a64-glibc-wl (guest, until round 5) | `surface`, `wayland_surface`, `get_surface_capabilities2`, `surface_maintenance1` | same |
| native (host), build-vk | `surface`, `get_surface_capabilities2`, `surface_maintenance1` (no platform) | same; build.sh now prefers `build-vk-x86-x11` when it exists |
| a64-glibc-wsi (new), build-vk-x86-x11 (new) | - | + `xcb_surface`, `xlib_surface`, `wayland_surface` |

gamescope itself never needs X11 WSI: it makes its own device and imports client images. For X11 clients the gamescope WSI layer (`VK_LAYER_FROG_gamescope_wsi`, upstream `layer/VkLayer_FROG_gamescope_wsi.cpp`) force-adds `VK_KHR_wayland_surface` and `VK_KHR_xcb_surface` to the instance, turns `vkCreateXcbSurfaceKHR`/`vkCreateXlibSurfaceKHR` into Wayland surfaces on gamescope's own Wayland display and makes a loader xcb surface only for windows that cannot bypass. So, with the layer, X11 swapchains already reach us as *Wayland* swapchains. The loader handles platform surface extensions itself, which is why the old guest build (no `xcb_surface` in the ICD) still enumerated and created devices under the layer. Without the layer (any process the layer does not wrap, `ENABLE_GAMESCOPE_WSI=0`, the host tests) an X11 client needs our own xcb surface support.

**How the vendor bridge serves X11 clients in the guest** (sources, not measured on poco-x4): `malibridge/client/wsi_x11.c` presents by software readback and `xcb_put_image` ("only when nothing better exists"); the main path is Wayland (`wsi_wayland.c`: AHB zero-copy into the DroidDeck compositor, or a copy; `VKBRIDGE_WSI_AHB`, `VKBRIDGE_WSI_SHM`). The DroidDeck guest's Xwayland has **no glamor and no DRI3** because there is no DRM node (`docs/development/mali-venus.md`, `VenusComponent.kt`: `MESA_VK_WSI_DEBUG=sw` is set in the guest so swapchains present through shared memory; Xwayland commits reused wl_shm buffers and gamescope patch 0927 uploads shm buffers on every commit). So the guest's X server most likely offers MIT-SHM, no DRI3, hence no dma-buf modifiers at all. `tests/x11guest.sh` records `xdpyinfo -queryExtensions` to settle it.

**Design options** (E = effort, R = risk)

| option | what | E | R |
|---|---|---|---|
| **(a) link Mesa's `wsi_common_x11`** (chosen) | build Mesa with `-Dplatforms=wayland,x11`, link `libloader_x11.a` + `libloader.a`, advertise `xcb_surface`/`xlib_surface`; wsi_common does the rest | S (one instance-table gate, one build flag, tests) | low: the same wsi_common that already carries Wayland; 0 Mesa patches |
| (a2) per-platform present path | two `wsi_device`s, routing each WSI entrypoint by surface platform so X11 can use software while Wayland stays dma-buf | M (~12 wrapped entrypoints) | medium/high: handle-keyed state, every wsi entrypoint touched, regresses easily |
| (b) own xcb surface + swapchain | our own `VK_KHR_xcb_surface`, present by shm/PutImage | M-L | medium: duplicates wsi_common, swapchain_maintenance1/present_id/present_wait all by hand |
| Mesa patch (`wsi->sw` per platform) | make `sw` a per-platform flag | S | rejected: 0-patch policy on Mesa |

**Present path** (chosen once per device in `mali_wsi_init`, `MALI_WSI` selects, Mesa's own `MESA_VK_WSI_DEBUG=sw` also forces software globally):

| `MALI_WSI` | path | default when |
|---|---|---|
| `sw` | CPU-visible linear buffer, MIT-SHM / PutImage; works with no dma-buf export, no DRI3 | fake backend |
| `dmabuf` | linear exportable BO, DRI3 `pixmap_from_buffer` (Wayland: `zwp_linux_dmabuf`) | hardware (default) |
| unset | as above | |

Caveat of (a): wsi_common's `sw` flag is global, so software mode also moves the *Wayland* platform to wl_shm. On hardware the default stays dma-buf (the gamescope-layer path, Wayland dma-buf, is the one that matters and is zero-copy); the software mode is for processes that are X11-only and not wrapped by the layer: set `MALI_WSI=sw` (or `MESA_VK_WSI_DEBUG=sw`, which DroidDeck already sets guest-wide) in them. Per-platform routing (a2) is the fix if both are ever needed in one process.

**Tests** (smoke set, host, fake backend, `check.sh` `x11 wsi host v10/v13`): `t_vk_xcb` (xcb window, `VK_KHR_xcb_surface`, support/capabilities/formats/present modes, FIFO swapchain, 3 x acquire/clear/present, teardown) and `glxinfo -B` with zink on our ICD under `gamescope --backend headless` (its Xwayland has DRI3, Present, MIT-SHM; the host has no Xvfb): **OpenGL 2.1 via zink**, renderer `zink Vulkan 1.3(Mali-G610 MC6 (mali-own, fake backend))`, no `could not create swapchain`. Negative check: `MALI_WSI=dmabuf` fails in `CreateSwapchainKHR` (the fake backend's BO fds are not dma-bufs and X rejects them), so the test does depend on the software path. Pixels: only the hardware variant `tests/x11guest.sh` (`t_vk_xcb --pixels` reads the window back with `xcb_get_image`; the fake backend executes nothing). Skips by itself when the ICD has no `xcb_surface` (Mesa tree without x11), without gamescope, or in the bionic build (no libxcb).

**The web helper (`+Vulkan` on the bridge) and the GL 3.0 blockers:** the web helper's arguments are `-cef-force-gpu -cef-use-gl=angle -cef-use-angle=vulkan` (`droiddeck-session`, with `BL_VKBRIDGE_CEF_VULKAN=0` by default): ANGLE **on Vulkan directly, not GL through zink**. So the zink GL 3.0 blockers (`EXT_transform_feedback`, `EXT_vertex_attribute_divisor`) do **not** apply to it; ANGLE emulates transform feedback with SSBOs and the instance divisor in its own translation (from memory, not verified here). What it does need on our driver is the surface/swapchain path (X11 through the layer to Wayland, or `xcb_surface` without the layer). The blockers still gate zink-GL clients: Steam's own GL (GLX, kopper) and Xwayland's glamor if one is ever present.

## Round 7: VK_EXT/KHR_vertex_attribute_divisor (historical: written when it was not yet advertised; it is now, divisor 0/1 only)
Needed by zink for GL 3.3 / ES 3.0 (`ARB_instanced_arrays`). Divisors 0 and 1 already worked (stride-0 trick); > 1 was rejected in `mali_pipeline.c`.
- **On the branch (09e0baa, `check.sh` OK):** `kb_draw_attr_instance` in libkbase (frequency, 1D POT type 2 with R, NPOT type 3 with R/E/D as panvk), host test `t_resources attr_instance`, shader `div.vert`, `t_vk_feat divisor` (skips while not advertised).
- **In `patches/vertex-attribute-divisor.patch` (needs the hot files + advertisement; host `check.sh` OK after `--bless`):** pipeline reject removed, `mali_attr.divisor`, emit call, extension/features/`maxVertexAttribDivisor`.
- **Confirmed on hardware:** nothing yet (phone at 56 C when tried). `t_vk_feat --only divisor` on v10/v11/v13 must pass before the patch lands.
- **Open:** firstInstance with divisor > 1 (hence `supportsNonZeroFirstInstance = false`).

## Round 8: the complete zink 26.0.8 requirement list (source of tag mesa-26.0.8; host-only, `t_steam_caps` checks it by name)
`tests/gen_zink26.py` generates `tests/zink26_table.h` from that tag's `zink_device_info.py` (108 device extensions) and `vk.xml`; `t_steam_caps` prints every one as ok/MISSING.
Nothing here is confirmed on hardware yet.
- Screen creation fails (hard), all HAVE: five extensions by NAME (`have_X` is name-only, the api version does not stand in): `VK_KHR_maintenance1`, `create_renderpass2`,
  `imageless_framebuffer`, `dynamic_rendering`, `descriptor_update_template`; `robustness2.nullDescriptor`; `timelineSemaphore`; calibrated timestamps, if advertised, must list
  `VK_TIME_DOMAIN_DEVICE` and 0 < domains < 8 (`t_steam_caps` checks both names of the entry point).
- The device request is every advertised table extension plus the queried feature chain: `t_steam_caps` makes that `vkCreateDevice` and resolves the 45 commands zink loads for the
  advertised extensions through `GetDeviceProcAddr` (KHR twin, then core name, as zink does): 0 unresolved.
- DRM path only (EGL/GBM, not the GLX/drisw path Steam's X11 clients use): `VK_EXT_physical_device_drm` (MISSING) and `VK_KHR_external_memory_fd` (have).
- `check_base_requirements` (warnings only): `logicOp` MISSING (`patches/zink-base-requirements.NOTE.md`), `shaderClipDistance` have, `scalarBlockLayout` have, `EXT_custom_border_color` have,
  `EXT_line_rasterization`: `patches/line-rasterization.patch` (rectangularLines only; `t_vk_feat linerast`).
- Optional extensions: 22 have, 81 missing of 103; 24 / 79 with the divisor and line-rasterization patches. They only select GL features. Missing ones that decide the GL level:
  transform_feedback, extended_dynamic_state 1/2/3, vertex_input_dynamic_state, push_descriptor, descriptor_indexing (core-promoted names count as missing: zink tests the name),
  draw_indirect_count, index_type_uint8, provoking_vertex, conditional_rendering, color_write_enable. See Round 5.
- Context creation cannot be predicted by name (dummy xfb buffer, descriptor init, first batch): that is a phone check.

## Round 9: zink on poco-x4 hardware (2026-11-18 08:41, head 7551451, glibc-wsi ICD sha 6b6d9e76, `MALI_WSI=sw`, steam-mode session, DISPLAY=:0)
**Hardware-confirmed:** `glxinfo -B` with `MESA_LOADER_DRIVER_OVERRIDE=zink` on our ICD (outside the vkbridge; only the test process sees it) **creates the zink screen and a context**: `direct rendering: Yes`, renderer `zink Vulkan 1.3(Mali-G610 MC6 (mali-own) (MESA_PANVK))`, OpenGL 2.1 (compat; core 0.0), ES 2.0, 3742 MB. Only message: `WARNING: ... base Zink requirements: feats.features.logicOp have_EXT_line_rasterization` (`zink_screen.c:2834`, warning only, not a refusal). That is the Round 5 GL level, so the next GL gate is not a failure but the missing optional extensions listed in Round 8.
- `eglinfo` needs `-p x11` (it has no positional platform); the GBM path still needs `VK_EXT_physical_device_drm`.
- A JSON whose `library_path` is on `/storage/emulated/0/...` fails with `ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)` (noexec fuse mount); the library must sit in the guest's own filesystem (`/tmp/x11wsi` persists across sessions).
**Steam burst: not on our driver.** `droiddeckctl env set` of `VK_DRIVER_FILES` is overwritten by the session script, and `BL_VK_DRIVER=/tmp/x11wsi/icd.json` is read (`== vulkan driver: imported /tmp/x11wsi/libvulkan_mali.so`) but the session then prints `== vulkan driver: Mali bridge` and gamescope, Steam and the web helpers run on the bridge (no process maps our library). Killing the session's `steam` ends the session. A real burst needs the bridge preference (`vkBridge`) off for that session: not tried.

## Round 10: Steam on our driver, poco-x4 hardware (2026-11-18, three bursts, `vkBridge` off, `MALI_WSI=sw`, glibc-wsi ICD)
Hardware-confirmed (bursts 1 and 2, head 7551451; burst 3 was the same stack):
- gamescope (Wayland backend) selects `Mali-G610 MC6 (mali-own)` and maps our `.so`; the Steam client (`steamrtarm64/steam`) maps it too (`/proc/<pid>/maps`).
- The Steam update UI, through zink plus the gamescope WSI layer, created a swapchain on our driver (3 images, 1638x720). `steamsysinfo` reports "Mali-G610 MC6 (mali-own)" / MesaPanVK. xdg-desktop-portal-gtk also ran zink on it.
- **Not reached: the Steam UI.** Every burst stayed in STARTING_STEAM ("Waiting for Steam's first frame"); nothing drawn, so no fps.
- **Blocker:** the web helper (`steamwebhelper`, `--use-angle=vulkan` GPU process) dies and is respawned about every 10 s: `GPU process exited unexpectedly: exit_code=8704`, then "Applying workarounds due to GPU process restarts: gpu:3" and "Triggering shutdown due to GPU process restarts"; also `shared_image_interface_proxy.cc(130) Buffer handle is null` and `one_copy_raster_buffer_provider.cc(348) Creation of StagingBuffer's SharedImage failed`. The GPU process lives under 2 s, so it was never sampled in `/proc`; the renderers never map our `.so`. `exit_code=8704` also occurs on the bridge baseline and recovers there, so the code alone is not proof of our driver; what differs is that on our driver the helper never recovers (12 reaps vs 0 on the bridge).
- Cause: **unknown.** ANGLE's init gates (vk_renderer.cpp @5d4df51d1d7d: instance/device >= 1.1, device count, queue family and memory type counts) do not fail for our 1.3 device on paper. Not verified on hardware.
- gamescope also logs (not fatal in these runs): `vkGetPhysicalDeviceFormatProperties2 returned zero modifiers for DRM format NV12`, `doesn't support VK_EXT_physical_device_drm` / `Failed to get DRM FD from renderer`, and zink's base-requirement warning (`logicOp`, `EXT_line_rasterization`; warning-only in zink 26.0.8).

Diagnostics added: `MALI_TRACE=<file>` (5aefcf1): load/unload, `vkCreateInstance`/`vkCreateDevice` with results, fatal signals with a backtrace; `t_vk_trace` checks it on the host. **Burst 3 with it produced no trace**: the file stayed empty. Unconfirmed why: my guest step that refreshes `/tmp/x11wsi/libvulkan_mali.so` printed no sha (its reply parsed wrongly), so the guest most likely still ran the previous ICD (no trace code). The next burst must verify the guest sha first. Burst 3 also stayed in STARTING_STEAM (screenshot: DroidDeck "Starting Steam" screen).

Next (needs a burst): refresh and sha-check the guest lib, rerun with `MALI_TRACE`; if the GPU process never loads our ICD, find why the web helper does not see `VK_DRIVER_FILES`; try `BL_STEAM_SOFT_COMPOSITING=1` (software CEF compositing) as a workaround.

## Round 11: rebase/merge, exit code 34, the web helper's crash is a SIGSEGV after `vkCreateDevice` (2026-11-18, burst 4 on poco-x4, glibc-wsi ICD sha 082ba324ac46, `MALI_WSI=sw`)
- **Merged to main** as 0bc0e77 (gated head 2b5aefd = the rebased branch + origin/main 2dfca98): smoke v10/v11/v13 pass (v13 ran on Mali-G925 MC11), v11 daily: the only new Fail is the known `a2r10g10b10_unorm_pack32_64x11_clamp_input_sample_count_4`. One non-golden conflict in the rebase (`mali_image.c` `view_refresh`, mechanical); none in `mali_pipeline.c`/`mali_cmd.c`/`mali_cs_emit.c`. `check.sh` now runs on a private empty `MALI_SHADER_CACHE`: main's own `vk_draw` goldens failed on a clean cache (stale cached compile, FS FAU `0x40` vs fresh `0x09000000`); re-blessed.
- **exit_code=8704 (verified, source):** `viz::ExitCode::RESULT_CODE_GPU_EXIT_ON_CONTEXT_LOST = 34` (`components/viz/service/gl/gpu_service_impl.h`, tag 126.0.6478.183); 8704 = 34 << 8, a wait status. So the browser saw the GPU process *exit on a lost context*, not fail to start. (The same code appears on the bridge baseline, where the process is relaunched and survives.) cef_log has no line before the exits: the helper's stderr is not captured outside the bridge branch of `droiddeck-session` (`BL_VKBRIDGE_CEF_LOG`).
- **Sandbox ruled out (verified from Steam's `Browser - launching child process` line):** the GPU process runs `--no-sandbox --use-angle=vulkan`.
- `gpu_memory_buffer_support_x11.cc(49)` = `dri3 extension not supported` (gamescope's Xwayland): explains `shared_image_interface_proxy ... Buffer handle is null` / StagingBuffer failures (one-copy raster has no GMB), not the exit.
- **Burst 4 (`MALI_TRACE`, verified: guest sha checked = host sha):** `steamwebhelper` loads our ICD (`ours=7`); a probe `vkCreateInstance` (engine "", 2 surface exts) is fine, then the real instance `engine=mesa zink api=1.3 exts=7` succeeds, `vkCreateDevice exts=27 queues=1 -> 0` succeeds, and the process takes **`FATAL signal 11`** right after, three times in 90 s (pids 28366, 28766, 29078), each the web helper's GPU-side process; the handler frame was all the backtrace showed (`mali_trace_fatal`, glibc `backtrace()` stops at the aarch64 signal trampoline), so *the faulting pc is unknown*: it may be in our ICD or in zink/Chromium after our device. gldriverquery/vulkandriverquery (32 and 64 bit), steamsysinfo, xdg-desktop-portal-gtk all create instance and device on our ICD without a crash.
- Steam-free repro, in-process GPU with ANGLE (`--use-angle=vulkan --in-process-gpu`, stderr logging): ANGLE's `vkCreateInstance` (api 1.1) returns ok, then the helper dies with `Trace/breakpoint trap` (a Chromium CHECK, 27-frame unsymbolized stack); the message above the stack was not captured (only the tail was printed).
- Fix landed for the diagnostics (569554a): `MALI_TRACE` fatal handler prints `si_code/si_addr`, pc, lr, sp and a frame-pointer walk with module+offset+symbol; `t_vk_trace` faults for real and checks addr and pc.
- Burst 5 (same, with the new handler and the full repro stderr) was aborted by the 50 degC skin rule at 52.9 degC (+67 s, started at 31.9) before the sampler output was read: nothing new. The trace file stays in the guest (`/tmp/x11wsi/trace.log`, `repro14.err`) until the next hold clears it.
- Next: one hold that first prints the previous `trace.log` and `repro14.err` (the burst-5 FATAL pc), starts from a cooler phone and stops at 40 s of Steam.

## Round 12: the helper's SIGSEGV is a NULL call inside the guest's libGLX_mesa, not in our driver (2026-11-18, burst 6 on poco-x4, glibc-wsi ICD sha e8526b55ef4d, `MALI_WSI=sw`)
- **Merged to main** as 98bfc13: `MALI_TRACE` fatal handler (si_code/si_addr, pc, lr, frame walk, and the `/proc/self/maps` line when the pc is outside every module), STEAM-GAPS rounds 10-11, `t_angle_seq`. Smoke v10/v11/v13 pass on b7a8faa (the merge with origin/main 38b61c7); 98bfc13 adds only origin/main's bench rows.
- **Offline (host fake backend, v10/v11/v13, ASan+UBSan clean): nothing found.** `t_angle_seq` replays what ANGLE (Chromium 126 `vk_renderer.cpp`, the 41 feature/property structs it chains, plus an unknown sType) and zink issue after `vkCreateDevice`: `vkCreateDevice` with all 37 advertised extensions, format/image-format/external-buffer/semaphore/fence queries for every handle type (including unknown ones), pipeline caches from real, truncated and garbage initial data, descriptor pools, samplers with custom border colour, exportable fence/timeline semaphore, external buffer/image. Every vk.xml command of every advertised extension plus core 1.0-1.3 (303 commands, `gen_procaddr.py` -> `procaddr_table.h`) resolves non-NULL through GIPA/GDPA. This rules the hypothesis "NULL entry point / unhandled pNext in our ICD" out for the 27-extension device the helper creates.
- **Hardware (burst 6, one hold, session 86 s, skin start 29.9 degC, peak 48.4 degC at +51 s, abort rule 50 degC not hit; restored: `vkBridge=true`, env empty, 0 leftover steam/gamescope, skin 36.5 degC afterwards):** the new handler caught the helper (pid 20315, `steamwebhelper`) right after its `vkCreateDevice exts=27 -> 0`:
  - `FATAL signal 11 code=1 addr=(nil)`, **`pc=0x0` (a call through a NULL function pointer)**, **`lr=libGLX_mesa.so.0+0x2a3d0`**, frames: `libSDL3.so.0` (+0x233d84, +0x233f48, +0x1286b0 ...) <- `steamwebhelper+0x1e810c ...` <- `libcef.so`. Our `.so` appears only as the handler frame itself.
  - So the faulting call is made from the guest's Mesa GLX (an SDL3 GL/GLX init path of the helper on gamescope's DRI3-less Xwayland), after zink created its Vulkan device on our ICD. It is **not** in `libvulkan_mali.so`; nothing in our driver can be fixed for it yet.
  - **Not verified:** the function name. The guest's `libGLX_mesa.so.0` is the droiddeck rebuild of Ubuntu resolute's mesa; the published builds 26.0.3, 26.0.8-1ubuntu0.3, 26.2.4 and 26.2.6 do not match (no `blr` before offset 0x2a3d0 in any of them), so the offset cannot be resolved offline. Resolving it needs the guest file itself (pull `/usr/lib/aarch64-linux-gnu/libGLX_mesa.so.0.0.0` in a ~15 s session without waiting for Steam's UI, then `llvm-objdump`/`llvm-nm`; also `dpkg -l libglx-mesa0` for the version).
- The old burst-5 files: `repro14.err` ends in Chromium's `Crashing due to FD ownership violation` + `Trace/breakpoint trap` (the in-process ANGLE repro in `/tmp`-less proot fds), an artefact of that repro, not the helper's SIGSEGV. No further repro hold is planned.
- Next: one 15 s session that only copies `libGLX_mesa.so.0.0.0` (and `libgallium_dri.so` version) to `/sdcard/Download`, then disassemble at lr-4 to name the NULL pointer (a loader/DRI extension table member). Likely suspects to check there: a DRI-loader callback the Xwayland/gamescope X server lacks (no DRI3/Present) while zink is the selected driver.

### Round 13: the helper SIGSEGV is in the guest's Mesa GLX, not in our ICD
- Guest rootfs (public `ubuntu-r1-rc5`, sha256 434d3d91...) fetched on the host, no session: Mesa 26.0.8-1ubuntu0.3+droiddeck1.
- `libGLX_mesa.so.0+0x2a3cc` is the `blr x1` in **`__glXGetDrawableAttribute`** (`glXQueryDrawable`), `case GLX_SWAP_INTERVAL_EXT`: **`psc->driScreen.getSwapInterval` is NULL** (`glx_pbuffer.c:315`, no guard; `glXGetSwapIntervalEXT` does guard).
- The hook is set only by `dri3_glx.c` and by `drisw_glx.c` when `psc->kopper`. The helper has `LIBGL_KOPPER_DISABLE=true` (session.log, burst 6) and no DRI3, so the screen is plain drisw and SDL3's swap-interval query crashes.
- Our ICD is not involved (`t_angle_seq` and the 303-entry procaddr table are clean). Fix: `patches/mesa-glx-null-swapinterval.patch` for the guest Mesa rebuild. Hardware test not run: it needs that rebuild.

### Round 14: who sets `LIBGL_KOPPER_DISABLE`, and a workaround that needs no Mesa rebuild
- **Who:** Valve's own `steamwebhelper.sh` (message "setting LIBGL_KOPPER_DISABLE=true for steamwebhelper to get store working properly") exports it for the client interface, unconditionally. Steam restores that script when it verifies its install, so it can't be edited. DroidDeck knows this: `tools/linuxfs/preload/venus.c` (in `libblsession`) drops the variable, but only when `VN_DEBUG` contains `vtest` (the Venus path), so on our open-driver path it stays set. `BL_OPEN_DRIVER=1` sessions get no such preload. The upstream tree also already calls this out in `docs/development/mali-venus.md` (item 8, DroidDeck issue #2243).
- **Workaround (a), tested on the X4:** `guest/nokopper.c` (a 15-line constructor, no libc, aarch64 `.so` of 3 KB) removes `LIBGL_KOPPER_DISABLE` from `environ` when `BL_OPEN_DRIVER` is set, without `setenv`/`unsetenv` (the guest guard refuses those). It is added to the guest's `/etc/ld.so.preload` after the session starts (the session rewrites that file at each start, so nothing persists). Host test: `tests/t_nokopper.sh` (real processes under `LD_PRELOAD`, gated on/off, similarly named variables kept, plus a mutation check that fails with the filter removed). Build for the guest: `clang --target=aarch64-linux-gnu -fuse-ld=lld -O2 -fPIC -shared -nostdlib guest/nokopper.c -o nokopper.so`.
- **Hardware (hold 19, session 119 s, skin 31.7 -> 43.1 degC peak at +28 s):** the helper started with the shim mapped (`shim=5` mappings, `BL_OPEN_DRIVER=1` in its environment) and **survived** past `STARTING_STEAM`: same pid (30133) at +28 s and +58 s, no FATAL from it, session state **READY**. The only crash in that window was the earlier helper (pid 29667) that started before the shim was in `ld.so.preload`, with the same `lr=libGLX_mesa+0x2a3d0` as round 12. This is consistent with the shim preventing the NULL-hook crash; it is one run and it does not show that Mesa read the filtered environment (no trace of that was taken). Restored: `vkBridge=true`, env empty, 0 leftover processes.
- **Not reached yet:** the library. The screenshot is the black session with the on-screen gamepad, and gamescope's wayland log shows 0 game frames. The helper's renderer (pid 30208) later died with `FATAL:child_thread_impl.cc(351) Crashing because hung` about 48 s after start, and there was no `--type=gpu-process` in the sample. Next: find out what the renderer waits for (its GPU channel). That is a different failure from the NULL hook.
- **Other holds of this round:** holds 17 and 18 only watched the Steam self-update (573 MB download, hot: skin 52.4 degC at +35 s, aborted by the 50 degC rule and restored). The update must finish once per fresh guest before the helper is reached; hold 19 was the first to get past it.

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
- `mali_pipeline.c`: pass `pSpecializationInfo` to `mali_compile` (gamescope needs it, see Round 2 result); vertex attribute divisor: the extension is advertised (`t_vk_feat divisor`) but only divisor 0/1; divisor > 1 stays unadvertised (see "Round 7"), immutable ycbcr samplers when pipelines are built.
- BO allocator: tiled/AFBC modifier images need dma-buf export of non-linear BOs and a stride/modifier plane layout; LINEAR works today.

## Risks
- gamescope forces compositing on the bridge (`gamescope_composite_force=1`); on our driver it will take the compute composite path, so shader compile coverage matters more than WSI.
- Not testable offline: Wayland WSI, dma-buf import from clients, present timing, every shader result (the fake backend does not execute).
- Smoke on the farm runs only `t_vk_draw`, `t_vk_compute`, `t_vk_meta`: extension-table changes are checked on hardware only by device creation.
