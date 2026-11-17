# Roadmap

Condensed from `ARCHITECTURE-proposal.md` (about 39 agent-weeks in the original estimate). Detailed plans: `R3-PLAN.md`, `R4-PLAN.md`.

| Stage | Goal | State |
|---|---|---|
| R0 | Zero-copy present, async submit, cached memory in the existing PanVK-kbase fork. Steam UI at 40 fps or more on the G615 phone | Out of this tree (fork work) |
| R1 | Standalone libkbase on both phones: hand-built command stream, dma-heap import, fences, 10k submits, injected fault decoded | Done on G615. G925 port runs but has seen less testing |
| R2 | Compiled-shader triangle through a zero-copy present at refresh rate | Done on G615 |
| R3 | Own Vulkan ICD: vkcube and dEQP-VK api/pipeline/renderpass at 95 % or more on v11 and v13 | **Current.** M1-M14 mostly done (draw, compute, meta, queries, WSI). Gate not run. dEQP sample: 272 pass, 136 fail |
| R4 | Vulkan 1.3 and the DXVK feature set. A DXVK title reaches a menu | In progress. Vulkan 1.3 device is up. DXVK-required features: 7 of 47 missing. BC decode wiring pending. Gate: dEQP within 2 % of the oracle |
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

dualSrcBlend, sampleRateShading, MSAA draw path, multiViewport, inlineUniformBlock, depth/stencil format gaps (D16, D32S8), BC wiring on hardware,
GPL (optional), a full dEQP run instead of the daily sample.
