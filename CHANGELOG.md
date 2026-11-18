# Changelog

History of mali-open generated from `git log` and merged PRs (2026-11-17 to 2026-11-18, 439 commits on main at the time of writing).
The project is pre-release (0.1.0), not conformant. dEQP-VK numbers come from the 1381-case daily sample on the G925 phone (Poco, v13) and are recorded in `vk/deqp/TRIAGE.md`.
Commit hashes are short SHAs. "Unreleased" covers everything so far; there are no tags yet.

## dEQP-VK daily sample over time (pass / fail / NotSupported / crash)

| When | Point | Pass | Fail | NS | Crash | Source |
|---|---|---|---|---|---|---|
| 11-17 | First run, first 782 cases only (`api.*`) | 38 | 3 | 632 | 108 | TRIAGE |
| 11-17 | After round 1 fixes | 195 | 126 | 1057 | 1 | TRIAGE round 2 |
| 11-17 | After round 2 | 221 | 101 | 1057 | 0 | TRIAGE round 2 |
| 11-17 | Round 3 baseline | 225 | 97 | 1057 | 0 | TRIAGE R4-M1 |
| 11-17 | R4-M1, Vulkan 1.3 device (`552913c`) | 270 | 134 | 975 | 0 | TRIAGE |
| 11-17 | R4-B2 re-run (`6332953`) | 272 | 135 | 972 | 0 | TRIAGE |
| 11-17 | R4-B3 (`4ff306e`) | 280 | 128 | 971 | 0 | TRIAGE |
| 11-17 | Job 1 regression fixed (`9e2a673`) | 283 | 126 | 971 | 0 | TRIAGE |
| 11-17 | Farm baseline reblessed, both phones (`1ef735b`) | 302 | 107 | 971 | 1 | commit message |
| 11-18 | Committed Poco baseline (`vk/deqp/ours-daily-sample-poco.tsv`) | 301 | 108 | 971 | 0 (1 other) | tsv |
| 11-18 | v13 daily on main `b0ea148` (farm job 20261118-172114, PR #30) | 425 | 27 | 929 | 0 | README |

Reference: PanVK oracle on the same list passes 642, fails 18, NotSupported 721, crash 0 (97.3 % of supported cases).
The README table carries this newest clean v13 run; the committed tsv baselines are older. No full CTS run and no R3 95 % gate run exist.
The 971 NotSupported cases count against the 2 % gate.

## Unreleased

### Vulkan features (R4: Vulkan 1.3 and the DXVK feature set)
- Vulkan 1.3 device with honest 1.1/1.2/1.3 property and feature tables (`8bd5e66`); DXVK-required features missing went from 23 to 5 in the R4-B2 batch.
- Added with a `t_vk_feat`/`t_vk_draw` test each: multiDrawIndirect, depthBiasClamp, drawIndirectFirstInstance, fullDrawIndexUint32, samplerAnisotropy, samplerMirrorClampToEdge, imageCubeArray, wideLines, shaderStorageImageExtendedFormats, bufferDeviceAddress, descriptor indexing (sampled-image subset), robustness2 nullDescriptor, scalarBlockLayout, uniformBufferStandardLayout, zero-initialize workgroup memory, occlusionQueryPrecise, shaderDemoteToHelperInvocation, subgroup ops (compute), subgroupSizeControl, shaderInt8/16/64, shaderClipDistance, shaderDrawParameters, shaderImageGatherExtended, fragmentStoresAndAtomics, vulkanMemoryModel, fillModeNonSolid, inlineUniformBlock.
- Specialization constants (`ec0de2e`), samplerYcbcrConversion for NV12 (`6d3f92e`), shaderStorageImageWriteWithoutFormat (`d5e606b`), VK_EXT_custom_border_color (`8e9410e`), VK_KHR_maintenance1/5, create_renderpass2, imageless_framebuffer (zink requirements).
- PR #10: rectangular line rasterization, vertex attribute divisor (0/1 advertised; >1 not, hardware divides differently), compute dynamic buffers.
- Also on main now: dualSrcBlend (blend shader), multiViewport (state only), depth clamp; MSAA exists behind `MALI_EXPERIMENTAL_MSAA=1`.
- Not done: default-on MSAA, sampleRateShading (env-gated), geometry/tessellation, logicOp, Bresenham/smooth lines.

### Graphics and render path
- Fixed-function state mapping: cull/front face, depth bias/clamp/clip, blend with masks and constants, dynamic state (`8e9410e` era, R4-A).
- Colour loadOp LOAD through a preload draw (`4b5d68d`); PR #11 extends it to RAW formats and CmdClearAttachments after a draw or on a sub-rect; PR #22 partial depth/stencil clears.
- Render target rows: blendable R8/RG8/BGRA8/sRGB/565/4444/5551/10-10-10-2/16F/11F, A8B8G8R8 pack32 (`9f76cbe`), A1R5G5B5 (PR #20).
- PR #17 FS-less pipelines (depth/stencil only); PR #18 sparse vertex attribute locations.
- Sampler fixes: wrap modes S/T/R mapping (U and W were swapped) and minLod (`f9580e8`); texture payload order layer-major (`6cba4ac`) fixing array layers >= 1 with mips.
- Blit/meta: generic blit shader path (M12), BC1-BC7 decode emulation verified on CMF.
- Crash fixes: tiler heap descriptor written once (>= 383 draws TRANSLATION_FAULT, `3d235f2`); geometry buffer 64 KiB on all arches (4 MiB broke v13); ClipDistance struct split (11 dEQP depth crashes); stack of live devices (OUT_OF_DEVICE_MEMORY in dEQP, `1c34dcf`); COHERENT_LOCAL kept for arch 11 meta allocations and tiler heap descriptor (`1842294`, `6bfa1cc`).

### Performance
- Async frame submit, up to 3 frames in flight (`4f60810`); batch all frames of a command buffer in one CS submission (`64edcc8`).
- PR #3: skip own-semaphore CPU wait when a dma-buf fence travels with present (X8 cube 720p acquire 7.4 -> 0.4 ms).
- PR #4 hot clusters; draw records shrink 1528 B -> small header plus pooled state blocks, dirty-mask state tracking (`54c2b7f`, `73afa37`); host 3k submit 0.317 -> 0.218 us/draw.
- Exported dma-buf memory is host-lazy (no per-submit MEM_SYNC, `1188485`; cached exports keep it, `e653a7a`).
- Draw-count scaling 3k/5k/10k/20k measured against the bridge (`595d6a9`). X8 parity runs: 5k draws 548 vs 278 fps, 10k 348 vs 152, 20k 185 vs 84 (worker-measured, see `bench/RESULTS.md` for published rows).
- Compute: allow_merging_workgroups and task increment (about -10 % per unit); timestamp period calibrated from GET_CPU_GPU_TIMEINFO (was 1 ns).

- PR #50: `MALI_OPT_GCM` env switch runs `nir_opt_gcm` on compute shaders only; off by default, `MALI_COMPILE_VERSION` 21 (output unchanged when unset).

### Steam / zink / WSI (STEAM-GAPS)
- X11 WSI (`40a2ece`): xcb/xlib surfaces, sw and dma-buf present path.
- zink 26.0.8 requirement list worked through (`ZINK_MISSING` 5 -> 3 -> 0 by name); vkCmdBindVertexBuffers3KHR; host zink smoke test.
- PR #6: web helper SIGSEGV root cause (guest Mesa GLX) plus `LIBGL_KOPPER_DISABLE` shim. Rounds 6-10 documented in `STEAM-GAPS`.
- `MALI_TRACE=<file>` post-mortem trace with fatal-signal frame walk (`569554a`); `MALI_IOCTL_PROF=1` ioctl profiling in libkbase.

### Tooling, farm and CI
- PR #1 progress diagram and CI; PR #2 CI link fix.
- Farm: PR #8 X4 guest route; PR #13 server-side phone holds and multi-job wait; PR #15 driver re-push, idempotent submit, merge-with-main, observed baseline, temperature gate; PR #19 bench UI-BLOCKED classification; PR #21 menu job deferral; PR #24 runaway-app guard and ALERTS line. Crash bursts cannot poison a daily (`f381acb`).
- `tools/inspect-session` (PR #12); qdeqp: `QDEQP_TMP`, `QDEQP_IMAGES=1` (PR #16).
- `vk/check.sh`: goldens run on a private empty shader cache; many offline host tests (`t_memsync`, `t_steam_caps`, `t_vk_trace`, ...).

- PR #48: event/query-pool BO owner fix, `vkFreeDescriptorSets` no longer double-frees a set already freed or from another pool, pool allocator callbacks honoured.
- PR #37: `vk/warnings.sh` extra-warning report (no link, nothing fatal) and the -Wextra/-Wshadow findings in libkbase and the ICD fixed. PR #39: `tools/coverage.sh` gcov function coverage of host tests on the fake backend, plus descriptor support/free cases.
- PR #31: tracked non-gating dEQP candidate case lists (`vk/deqp/cases-small.txt`, `cases-deep.txt`, generated by `gen-caselists.py`). PR #101: `.gitignore` covers the local `third_party`, `vk/deqp/lists`, `vk/deqp/out` symlinks.
- PR #49: `tools/bench2` ranks the presented config by fps (`RANK_VER` 2) and has 5k/10k/20k draw-count triangle variants.

### Benchmarks
- CMF and Poco ours-vs-bridge results, mdbench csv with GPU start/end and present timeline (PR #9), performance charts and README table regenerated on publish (PR #5), backfilled RESULTS rows with skipped-row logging (PR #7).

### Docs
- README, ROADMAP, LICENSE-NOTES, R3/R4 plans, TRIAGE rounds; PR #23 README status and limits; PR #25 DXVK-GAP re-verified against DXVK/VKD3D-Proton source; PR #38 `CONTRIBUTING.md`; PR #82 `docs/REVIEW-CHECKLIST.md` from recurring review findings; PR #77 README/ROADMAP/CHANGELOG corrected against main; PR #94 stale `STEAM-GAPS` statements fixed.

## Foundations (2026-11-17 snapshot `fe0c5bb`)
- libkbase (kernel base driver user space) and the Vulkan front end milestones M1-M14 (M12/M13 wired), with plans. Own Vulkan ICD for Mali-G610 (v10), G615 (v11) and G925 (v13).
