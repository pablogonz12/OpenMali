# Unity 2023.2 (Vulkan 1.1) on our driver: verdict per capability

Host-measured, no phone. Sources: `/tmp/oc/unity-vulkan-caps.md` (a real `VkDeviceCreateInfo` dump from Megabonk's
`-force-vulkan` run on radv) for what Unity asks, and our own driver for what it gets:

* `tests/t_unity_caps.c` (`tests/build-t_unity_caps.sh [v11|v13]`, wired into `check.sh`): every format Unity hands to
  `vkCreateImage` through `vkGetPhysicalDeviceFormatProperties`, `vkGetPhysicalDeviceImageFormatProperties` **and** a real
  create/bind (mip chain, cube), plus the swapchain contract on whatever surface the build has.
* Swapchain rows: the same test against the `-Dplatforms=x11` tree (`MESA_BUILD=third_party/build-vk-x86-x11`,
  `MALI_WSI=sw`) inside `gamescope --backend headless`, i.e. a real Xwayland X server, as `tests/xwsi-host.sh` does.
* `tests/t_compile.c` `dyn_index` + `MALI_NIR_DUMP`: the lowered NIR for a runtime index into UBO/SSBO/storage-image arrays.

Verdict legend: **hard blocker** = the game cannot start or the first frame is black/corrupt; **degrades** = a logged
fallback or an unused code path; **fine** = measured, nothing to do.

## Formats

| what | verdict | evidence |
|---|---|---|
| Colour render targets: B8G8R8A8/R8G8B8A8 UNORM+SRGB, A2B10G10R10, A2R10G10B10, B10G11R11, E5B9G9R9, R16G16B16A16 (SFLOAT/UNORM/SNORM), R32G32B32A32, R8/R8G8/R16/R16G16 (UNORM/SFLOAT), R32/R32G32, R8/R16/R32/R8G8/R16G16/R32G32/(R32G32B32A32) UINT, R5G6B5, A1R5G5B5 | **fine** | created and bound; colour-attachment + blend bits where Unity blends |
| Depth/stencil: D16, D24S8, D32_SFLOAT, D32_SFLOAT_S8, S8, X8_D24 | **fine** | created and bound, depth/stencil-attachment + sampled |
| BC1-BC7 (all 16 formats) | **fine** | `textureCompressionBC` advertised, `mali_bc_optimal_features()` gives SAMPLED + FILTER_LINEAR + BLIT_SRC + TRANSFER_SRC/DST, and every one of the 16 is created, bound and cube/mip created; decode-to-shadow is `mali_bc.c` (R4-M7) |
| A4B4G4R4_UNORM (`VK_EXT_4444_formats`) | **degrades** | we do not advertise the extension, and the format is not creatable (`VK_ERROR_FORMAT_NOT_SUPPORTED`, -11). Unity only uses it when the extension name is present, so the fallback path is taken; one logged `GraphicsFormat ... not supported` line at worst |
| ETC2/EAC, ASTC | fine (not used by this player) | this build enables `-vulkan-force-disable-ASTC-support`/`-ETC-support`; BC is the requested family |

## Descriptor indexing

| what | verdict | evidence |
|---|---|---|
| `shaderUniformBufferArrayDynamicIndexing` | **fine**, advertised | lowering: the variable index becomes `vulkan_resource_index`/`load_vulkan_descriptor` (clamp against `count - 1`) and `+ idx * stride` on the first handle; `t_compile dyn_index` reads the clamp from the dumped NIR (count 4 → 3, count 8 → 7) and asserts the index is still dynamic. Mutations that drop the index or the add make it fail |
| `shaderStorageBufferArrayDynamicIndexing` | **fine**, advertised | same path; same test (`ssboidx`), the clamp constant rides in the FAU immediates, so the test compares code **and** immediates |
| `shaderStorageImageArrayDynamicIndexing` | **fine**, advertised | storage images are one 32-byte slot per element (stride 1), `deref_handle()` folds the index into the image handle; `imgidx` asserts the deref is gone and an `iadd` carries the index. Dropping `if (ssa)` in `deref_handle()` makes the test fail |
| `*ArrayNonUniformIndexing` (the 1.2 set) | **not advertised** | the waterfall lowering exists and `t_vk_feat didxbuf` exercises it, but nothing pixel-checks it on hardware yet. Unity 2023.2 does not request these bits |
| `shaderStorageImageReadWithoutFormat` | not advertised | Unity asks for it; write-without-format is advertised. No shader in the measured set needs the read form without a format; if one appears it is a per-pipeline failure, not startup |

## Swapchain (measured on the xcb surface, fake backend, gamescope's Xwayland)

| what | measured | verdict |
|---|---|---|
| surface extensions | `VK_KHR_surface` + `VK_KHR_xcb_surface` (+ wayland/xlib in their builds) | **fine** |
| surface formats | `B8G8R8A8_SRGB`, `B8G8R8A8_UNORM` — the pair Unity wants (RGBA8 is not offered) | **fine** |
| present modes | IMMEDIATE, MAILBOX, FIFO, FIFO_RELAXED | **fine** (FIFO present) |
| composite alpha | OPAQUE + INHERIT | **fine** |
| images | minImageCount 3, `maxImageCount` 0; Unity's default 3 requested → 3 returned | **fine** |
| sRGB view (`vulkanEnableSetSRGBWrite`) | `VK_KHR_swapchain_mutable_format` advertised; swapchain created with MUTABLE_FORMAT + `{B8G8R8A8_UNORM, B8G8R8A8_SRGB}` format list, SRGB view created and destroyed | **fine** |

## Remaining rows from the caps document

| what | verdict |
|---|---|
| `VK_KHR_swapchain`, 1 graphics queue with present, Vulkan 1.1 instance/device | fine |
| `KHR_create_renderpass2`, `bind_memory2`, `get_memory_requirements2`, `dedicated_allocation`, `descriptorUpdateTemplate` | fine |
| Memory: host-visible-only types, suballocator, `memory_budget` | fine (warning-level only) |
| `geometryShader`, `tessellationShader`, `shaderCullDistance`, `vertexPipelineStoresAndAtomics`, `shaderFloat64`, `logicOp` | **degrades** (per-pipeline failures only if a variant uses them; Unity drops the bits, so it also drops those variants) |
| `sampleRateShading` 4x MSAA | degrades (1x unless the app asks for 4x, which is on by default; `MALI_EXPERIMENTAL_MSAA=0` opts out) |
| Sparse binding/residency, storage-image multisample, EXT_4444, conservative rasterisation, fragment shading rate, HDR metadata, load-op-none, memory priority, viewport-index-layer, external memory host, driver properties | degrades (silent code-path loss) |
| MRT (4 colour attachments, one attachment renders) | **degrades** (render targets 1..3 wrong/black), not startup |

Bottom line: no hard blocker in the format, dynamic-indexing or swapchain paths. The only degraded format row is
`VK_EXT_4444_formats`; the biggest remaining functional risk is multi-render-target rendering, which is a drawing-correctness
problem rather than a launch failure.
