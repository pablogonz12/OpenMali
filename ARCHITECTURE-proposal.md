# Own Mali Vulkan driver: architecture proposal (2026-11-15, not yet confirmed by the owner)

Decision proposed: own out-of-tree C11 driver that owns everything hardware/platform-specific; link a pinned, unmodified
Mesa (26.3) for the shader compiler and Vulkan plumbing. No own shader compiler. PanVK and the kbase fork are oracles only;
nothing copied from src/panfrost/vulkan; <~20 local Mesa patches; reuse stated openly.

| Layer | Decision |
|---|---|
| libkbase (CSF backend, no Mesa dep) | Own. uAPI version table (1.21 on CMF). Submit via CS_INSERT + mapped doorbell, ioctl kick only when off-slot. KCPU queue for fences. Decode fault notifications (no dmesg un-rooted). |
| Memory | Own. Cached kbase memory for CPU-read data, uncached for write-only streams, dma-heap + MEM_IMPORT for shared. |
| Sync | Own vk_sync type on sync_file/CQS + async submit thread. |
| Command stream v11/v13 | Own emission; Mesa genxml for descriptor packing. |
| Shader compiler | Reuse spirv_to_nir + NIR + Mesa Valhall backend behind our small interface. A/B old compiler vs Kraid (default in 27.0). |
| Vulkan front end | Own cmd buffers, descriptors, pipelines/GPL, GS/tess/xfb/BC emulation; reuse vk_* runtime. |
| WSI | Reuse wsi_common protocol; own image allocation (dma-heap import), fences, gamescope→Android leg. |

Where it can win: vs bridge on Poco (CPU-bound): no round trips/serialization → 1% lows, latency; features (GPL, BC) → compatibility,
less shader stutter. Vs bridge on CMF (GPU-bound): parity + a few %. Vs PanVK-kbase: presentation (Steam UI), async submit, pacing, v13.

Ladder (≈39 agent-weeks): R0 on the existing fork: zero-copy present + async submit + cached memory, Steam UI ≥40 fps on CMF (2).
R1 standalone libkbase on both phones: hand-built CS clears an imported dma-heap buffer, readback matches, fence signals, 10k submits
clean, injected fault decoded (3). R2 compiled-shader triangle through gamescope zero-copy at refresh (3). R3 vkcube + dEQP-VK
api/pipeline/renderpass ≥95 % on v11+v13 (6). R4 Vulkan 1.3 + DXVK feature set, a DXVK title reaches menu (8). R5 emulated GS/tess/xfb,
Megabonk matches oracle screenshots (6). R6 Megabonk on CMF ≥ bridge with fewer >50 ms frames; Steam UI ≥ bridge on both (5).
R7 vkd3d-proton title in gameplay, 2 h soak, 10-title matrix ≥ bridge (6).

Kill/pivot: (1) if R0 alone gives bridge-class UI + Megabonk parity, the own front end buys ownership/features, not performance.
(2) >1 copy per frame left after R2 → present through the bridge only. (3) uAPI drift beyond version-table data in >2 places → freeze
supported firmware list. (4) after R4 dEQP >2 % below oracle or Megabonk misrenders → transplant our lower layers into the fork.
(5) GPU time/frame on CMF within 5 % of blob but fps below bridge → stop tuning, reposition on compatibility.

Next measurements: DMA_BUF_IOCTL_IMPORT/EXPORT_SYNC_FILE on both kernels; GPU-active ms/frame + frequency residency on CMF (unplugged,
same scene) for bridge / fork old compiler / fork Kraid; probe the Poco (uAPI, arch, TEXTURE_FEATURES) + fork-vs-bridge Megabonk there.
