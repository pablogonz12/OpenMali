# Roadmap

Condensed from `ARCHITECTURE-proposal.md` (about 39 agent-weeks in the original estimate). Detailed plans: `R3-PLAN.md`, `R4-PLAN.md`.

| Stage | Goal | State |
|---|---|---|
| R0 | Zero-copy present, async submit, cached memory in the existing PanVK-kbase fork. Steam UI at 40 fps or more on the G615 phone | Out of this tree (fork work) |
| R1 | Standalone libkbase on both phones: hand-built command stream, dma-heap import, fences, 10k submits, injected fault decoded | Done on G615. G925 port runs but has seen less testing |
| R2 | Compiled-shader triangle through a zero-copy present at refresh rate | Done on G615 |
| R3 | Own Vulkan ICD: vkcube and dEQP-VK api/pipeline/renderpass at 95 % or more on v11 and v13 | **Current.** M1-M14 mostly done (draw, compute, meta, queries, WSI). Gate not run. dEQP daily sample (1381 cases): 425 pass / 27 fail / 929 NotSupported (Poco X8, main `b0ea148`); committed baselines are older, 301 / 108 / 971 |
| R4 | Vulkan 1.3 and the DXVK feature set. A DXVK title reaches a menu | In progress. Vulkan 1.3 device is up. DXVK-required features: 5 of 47 missing (`DXVK_MISSING=5` in `vk/check.sh`). BC decode wiring pending. Gate: dEQP within 2 % of the oracle |
| R5 | Emulated geometry, tessellation and transform feedback. Target title matches oracle screenshots | Not started |
| R6 | Target title on the G615 phone at least as good as the bridge, with fewer frames over 50 ms. Steam UI at least as good on both phones | Not started |
| R7 | vkd3d-proton title in gameplay, 2 h soak, 10-title matrix | Not started |

## Kill and pivot rules

1. If R0 alone gives bridge-class results, the own front end buys ownership and features, not speed.
2. More than one copy per frame after R2: present through the bridge only.
3. uAPI drift beyond version-table data in more than 2 places: freeze the supported firmware list.
4. After R4, dEQP more than 2 % below the oracle or the title misrenders: move our lower layers into the fork.
5. GPU time within 5 % of the vendor driver but fps below the bridge: stop tuning and reposition on compatibility.

## Open items before R4 closes

Facts from `vk/check.sh` (`DXVK_MISSING=5`), the feature table in `vk/src/mali_physical_device.c` and `vk/tests/t_dxvk_caps.c`.
Done on main: dualSrcBlend, inlineUniformBlock, multiViewport, nullDescriptor, depthBiasClamp, fillModeNonSolid, independentBlend, BC texture feature.

- DXVK-required features not yet set on main: sampleRateShading (env-gated MSAA), shaderCullDistance, robustBufferAccess, descriptorIndexing, and
  geometryShader (must stay rejected until R5; `t_vk_info` checks that). Re-run `t_dxvk_caps` for the exact list before starting one.
- Experimental on main: MSAA behind `MALI_EXPERIMENTAL_MSAA=1`. Check open PRs for stencil and D32S8/D24S8 depth-stencil formats.
- DXVK extensions in `t_dxvk_caps` still missing from the extension table: EXT_graphics_pipeline_library and KHR_pipeline_library (GPL is optional). Advertised since the last roadmap: EXT_extended_dynamic_state3 (polygonMode only), EXT_depth_clip_enable, KHR_maintenance5/6, KHR_load_store_op_none, EXT_vertex_attribute_divisor, EXT_memory_budget, KHR_calibrated_timestamps, EXT_custom_border_color, EXT_robustness2 (nullDescriptor only), KHR_swapchain.
- BC decode wiring on hardware (offline tests pass, `t_bc_hw` exists).
- A full dEQP run instead of the daily sample (929 are NotSupported, which counts against the 2 % gate).
