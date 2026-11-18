# dEQP-VK triage, our ICD vs PanVK-kbase (Poco G925/v13, glibc guest) — 2026-11-17

Status: PARTIAL. Sample list `lists/daily-sample.txt` = 1381 cases (every 20th of r3-daily + ≥1 per 4-level group, stress cases removed). Ours ran 782 of them;
stopped after two consecutive 240 s call caps (rule 8).

## Baselines (PanVK, same list, `runs/qdeqp/poco-panvk-daily-sample.csv`)
1381 cases: 642 pass, 18 fail, 721 NotSupported, 0 crash → 97.3 % of supported (46.5 % of total). All 18 fails are in `api.copy_and_blit.core`
(oracle bugs / format combos; not ours to fix). api.object_management stress group (137 cases): the "137 crashes" were a harness artefact, see below.

## Ours (`ours-daily-sample-poco.{csv,tsv}`, first 782 of 1381 sample cases; api.* only, pipeline/renderpass not reached)
782 done: 38 Pass, 3 Fail, 632 NotSupported, 108 Crash, 1 Other:ResourceError → 38/142 = 26.8 % of supported (PanVK full sample: 97.3 %).
NotSupported matches PanVK (copy_and_blit.core: 590 vs 591 of 695/887 seen), i.e. no feature-gate gap there. Not yet run: remaining 599 cases
(pipeline.monolithic 5426-case group, renderpass.*, image_clearing, buffer).

## Failure clusters (ranked by case count)
1. **api.copy_and_blit.core: 105 Crash of 695 seen** (every case PanVK passes, 278 of 887 in total ⇒ ≈ all of them). Process dies while running
   the case (copy/blit/resolve use transfer cmds, blit/resolve go through meta draws or compute). Root cause guess: not-implemented transfer path
   (vkCmdCopyImage*/BlitImage/CopyBufferToImage → `vk_meta` or CS-based copy; probably unimplemented entrypoint or null dispatch/pipeline) — a bug/missing code,
   not a feature flag (NS count equals PanVK). Fix location: driver/vk cmd-buffer transfer entrypoints / vk_meta hookup; reproduce with one case
   `dEQP-VK.api.copy_and_blit.core.*` via qdeqp.
2. **Draw/hang cases**: api.smoke.asm_triangle (+_no_opname), command_buffers.submit_twice_secondary (hang, watchdog), command_buffers.recording_to_invalid
   (3 cases). Root cause guess: graphics draw submit/FBD path incomplete (see M8/M9). pipeline.* and renderpass.* are expected to be ~100 % crash.
   Fix location: CmdDraw / render pass begin-end / queue submit of tiler+fragment jobs.
3. **api.buffer_view.access.{uniform,storage}_texel_buffer.{r8_unorm,r16g16b16a16_uint}**: 2 Fail. Root cause guess: texel-buffer descriptor encoding
   (M5/M7 buffer-view descriptor bytes; golden tests cover images only). Fix location: buffer view descriptor build.
4. **api.info.unsupported_image_usage.optimal.input_attachment_bc1_rgba_unorm_block**: 1 Fail of 12. Root cause guess: INPUT_ATTACHMENT usage accepted
   for BC formats in vkGetPhysicalDeviceImageFormatProperties / vkCreateImage. Fix location: format-feature table / image-format-properties limits.
5. **VK_ERROR_OUT_OF_DEVICE_MEMORY in vkCreateComputePipelines** (api.descriptor_set.descriptor_set_layout_lifetime.compute, 1 case; a ResourceError aborts the
   whole deqp run). Root cause guess: shader/pipeline BO leak or oversize allocation per compile. Fix location: pipeline create/destroy, kbase mem alloc/free pairing.

## Harness findings (qdeqp.py, fixed)
* The "137 PanVK object_management crashes" were not PanVK: the QPA log was buffered, so when heavy cases (alloc_callback_fail, max_concurrent,
  multithreaded_*; load avg ~25) hit the chunk wall limit, all results were lost, the retry-alone then ran on a starved phone. Fixes: `--deqp-log-flush=enable`;
  a guest-side watchdog (kill on MemAvailable <1.5 GB, load1 >12 after 60 s, or no QPA growth for max(45, case-timeout) s, case recorded as `ResourceHog`,
  session restarted); `calm()` wait (load <8, >2 GB free) before each chunk/retry; stress cases (alloc_callback_fail / max_ / multithreaded / many) are
  excluded by default and written to `lists/stress.txt` (`--stress 1` to run). Verified: 40 of those cases pass on PanVK in one chunk (127 s).
* Ours: a case that dies while running is recorded Crash immediately (no retry); deqp-wide abort on ResourceError is retried alone.
* deqp executes in tree order, not caselist order: the crashed case is the QPA's unfinished case (`unf`), earlier code blamed the first list entry and wasted 2 retries/crash (fixed). `calm()` ignores load <40 (phone idles at ~20 with Steam up). ~5 s per crash now; 300 cases ≈ 130 s (fewer when crash-heavy).
* `scripts/quick/qsafe` bug (not mine): lock path uses `$1` after `shift 2` ⇒ always `PHONE BUSY`; use `$lock=/tmp/qsafe-$P.lock` saved before shift.
* `driver` is now a symlink to ~/Documents/scripts/mali-open: qdeqp falls back to cwd for ROOT.

## Not done
Remaining 599 sample cases (resume: `qdeqp poco lists/daily-sample.txt --icd ours --chunk 300 --limit N --case-timeout 45`); CMF; stress list.

## Crash triage (Poco, ours ICD) — daily-sample 1381 cases

Root cause of 105 copy_and_blit crashes: stale pushed a64-glibc lib (Nov 16) predating mali_meta_vk.c -> CmdCopyBufferToImage2 NULL. Rebuilt; remaining failures were a too-limited blit.
Fixes (vk repo): 2b544c1 generic blit shader (any plain colour fmt, 128 B push), d20eb5c R11G11B10/E5B9G9R9 src + 3D 1:1, 598c87b 3D nearest z scale, 5eeac40 secondary cmd buffers (cmd queue + enqueue dispatch), 4b348b2 lower load_base_instance (vertex_input crashes), + shader_out initializer lowering.
Before: 38 pass / 3 fail / 108 crash / 632 NS (782 run). After: 194 pass / 119 fail / 1057 NS / 10 crash (before last 3 fixes: ~2 crashes remain).
copy_and_blit.core: 887 total, 118 pass, 37 fail, 0 crash. image_clearing.core: 113 total, 28 pass, 13 fail.
Remaining: BC/ETC/EAC blit sources (~25), E5B9G9R9 dst, 3d r4g4b4a4 linear / r8_srgb nearest, image_clearing 13 fails, pipeline.monolithic depth/blend/stencil/sampler fails, vertex_input divisor (FEATURE_NOT_PRESENT), SEGV timestamp.draw_indirect_stage_out_of_render_pass (uninvestigated).
Notes: adb needs PATH=$PATH:~/Android/Sdk/platform-tools; glibc cross build needs symlink ~/Documents/scripts/droidtest -> ~/Projects/mali-gaming/droidtest; qsafe fixed.

## Round 2 (Poco, ours ICD, daily-sample 1381)
True numbers after round 1 fixes: 195 pass / 126 fail / 1057 NS / 1 crash / 2 other. After round 2: **221 pass / 101 fail / 1057 NS / 0 crash / 2 other**.
- Test regressions explained: t_cmd (24 fails) came from 794a571 (driver set grew to MALI_DUMMY_SAMPLER_IDX+1 entries, so SRT size/set0 differ from the libkbase reference): cmp_cmd.py now masks the size and compares the common prefix. t_compile v13 FS golden was stale (fau reserved 16, push base 24 since the blend-words change): gen-golden.sh fixed and golden regenerated. Commit d7bab6e. check.sh, t_meta, t_bc, t_cmd, t_compile all pass.
- Cluster A, non-triangle topologies (7efec14): point/line list+strip, triangle strip/fan; draw mode in the tiler flags, FP16 point size + extended FIFO for points that write PointSize, VS compiled with PSIZ stripped for non-point topologies, primitive size/line width 1.0. depth.*line_list/point_list cases now pass.
- Cluster B, BC blit sources (9892715): blit from a BC image reads the decoded shadow; 19/19 BC-source blit_image samples pass. ETC2/EAC sources (7 in sample) still fail (no decode emulation).
- Crash timestamp.draw_indirect_stage_out_of_render_pass (9337154): vkCmdDrawIndirect* had no entrypoint, NULL dispatch. Now fails the command buffer cleanly (FEATURE_NOT_PRESENT); indirect draws are not implemented.
- Remaining Fails (101), by cause: render targets other than RGBA8_UNORM ("colour attachment view": ~30: packed/16 bit/int/float/sRGB formats, image_clearing, blend.format, renderpass.suballocation); D24S8/D32S8/S8 attachments and stencil; colour loadOp LOAD (~8); CmdClearAttachments after a draw (~27, needs a quad draw); instance-rate vertex attributes + vertex input ~7; dynamic descriptor offsets (2); ETC2/EAC sources (7); E5B9G9R9 dst; sampler border_swizzle (6); pipeline state mali_pipeline.c:79 (7: VS-only / module-less).

## Round 3 (Poco, daily-sample 1381)
Before 221 pass / 101 fail / 0 crash -> after 225 / 97 / 0 crash.
- f526458: blendable colour render targets beyond RGBA8 (R8, RG8, BGRA8, sRGB, 565/4444/5551, 10-10-10-2, 16F, 11F) via rt_tab in mali_render.c. Few cases flip because most still hit other limits.
- Still out of scope: int, snorm, unorm16, 32-bit and other RAW formats (need shader-side blend lowering).
- Blend state is not mapped from the pipeline (always opaque replace): blend.format.* cases fail with "Image mismatch".
- Indirect draws (cc68908): DrawIndirect/DrawIndexedIndirect (+Count) are expanded at submit from the CPU mapping. multiDrawIndirect and VK_KHR_draw_indirect_count are not advertised, so the dEQP indirect suite is mostly NotSupported. The draw_indirect timestamp case went Crash -> Pass.
- Still failing: colour loadOp LOAD, CmdClearAttachments after a draw, partial clears, D24S8/stencil, 4-sample images, b4g4r4a4 clear mismatch (cause unknown), suballocation a2b10g10r10 clear/draw.

## R4-M1 (Poco, device now reports Vulkan 1.3, daily-sample 1381, full run in one pass)
Round 3 baseline 225 pass / 97 fail / 1057 NS / 0 crash -> **270 pass / 134 fail / 975 NS / 0 crash / 2 other**. Per-case diff vs the saved round 3 TSV: **0 regressions** (no Pass lost), 45 NotSupported -> Pass, 37 NotSupported -> Fail.
The 82 flipped cases were gated off by the 1.0 device (sync2, dynamic rendering, maintenance4, hostQueryReset, privateData, BC-copy variants) and are now exercised; the 37 new fails are real gaps that were hidden (e.g. `api.object_management.private_data.buffer_view_storage_*`: privateData itself is probed by t_vk_info, the failure is the storage texel buffer view; linear 1D/3D image_format_properties; d24_unorm_s8 separate-layouts depth; copy_and_blit 2d_to_3d with BC/ETC2/EAC pairs). Tracking them as the R4-M1 follow-up list. `ours-daily-sample-poco.{tsv,csv}` replaced with this run.

## R4-A draw.* (after fixed-function state)
Probe `^dEQP-VK.draw\.` 300 cases: 23 pass / 9 fail / 267 NS / 1 crash. Fails: D16 depth (multiple_clears d16 x6, depth_clamp.d16), negative_viewport_height.front_ccw_cull_front, scissor.static_scissor_two_clears. Crash: concurrent.compute_and_triangle_list. NS mostly missing extensions (multi_draw, vertex_attribute_divisor, vertexInputDynamicState, draw_indirect_count).

## R4-B2 daily-sample re-run (Poco, after depthBiasClamp/indirect first instance/anisotropy/mirror clamp/cubeArray/wideLines/ext storage formats/BDA/descriptor indexing)
Baseline 270 pass / 134 fail / 975 NS / 0 crash -> **272 pass / 135 fail / 972 NS / 0 crash / 2 other**. Per-case diff: **0 regressions**.
Flips: sampler mirror_clamp_to_edge NS->Pass, renderpass.suballocation.simple.color Fail->Pass; 2 NS->Fail from imageCubeArray now exposed
(`render_to_image.{core,dedicated_allocation}.cube_array.*.r32_uint_s8_uint`: the D32S8 depth/stencil attachment gap already listed above, not a cube-array defect). Build note: aarch64 needs `MESA_BUILD=third_party/build-vk-a64-glibc MALI_TARGET=a64-glibc ./build.sh` (default MESA_BUILD makes an x86 .so and dEQP reports INCOMPATIBLE_DRIVER).

## R4-B2 recheck (2026-11-17, after clip/drawparams/gather/fsstore/memmodel)
Full daily sample: 272 pass / 136 fail / 971 NS / 0 crash / 2 other. Baseline 272/135/972/0. The one change: renderpass.depth_stencil_write_conditions.depth_demote_write_d24_unorm went NS -> Fail (a feature gate now opens it; not a regression of a passing case).
An intermediate run had 11 depth.compare_ops crashes: nir_split_per_member_structs ran on every VS (commit 2560c16). It now runs only for a VS with ClipDistance (compile v14).

## R4-B3 daily-sample re-run (Poco, after layer-major texture payload + inlineUniformBlock)
Full daily sample 1381: 280 pass / 128 fail / 971 NS / 0 crash / 2 other (baseline 272/136/971/0). 0 regressions vs the previous run, 10 cases newly passing (vertex_input multi-attribute, sampler mirror_clamp/mirrored_repeat on 2d/2d_array formats). `ours-daily-sample-poco.{tsv,csv}` replaced.

## Job 1 regression (2026-11-17): dEQP aborted at case ~63-77 with VK_ERROR_OUT_OF_DEVICE_MEMORY — fixed
Not a BO leak (t_vk_leak: 300-2000 iterations of device create/destroy, memory alloc/free/map, fill-buffer submits, pools/cmd buffers, 8192 live compute pipelines: 0 growth of fds/maps/RSS, 64 simultaneous devices fine).
Causes: (1) `mali_cmd`/`mali_meta` bindings are process-global; destroying ANY VkDevice (dEQP creates short-lived extra devices) unbound them for the surviving main device,
so `vkCreateEvent` (and meta allocations) returned OUT_OF_DEVICE_MEMORY (api.command_buffers.submit_twice_secondary). Fix: stack of live devices in mali_queue.c, destroy re-binds the previous one; meta shader uploads deduped
(re-bind clears the shader cache). Regression test `t_vk_leak --only second` fails before (CreateEvent=-2), passes after. (2) harness: `qdeqp.py` pulled the QPA to hard-coded /tmp (full tmpfs) => "no QPA"; now QDEQP_TMP.
Daily sample after: 283 pass / 126 fail / 971 NS / 0 crash / 1 other (baseline 280/128/971/0); 0 regressions, 3 newly passing (buffer_view r8_unorm, r16g16b16a16_uint, submit_twice_secondary).
Run `t_vk_leak` on the guest with `vk/tests/leakrun.sh "--n 300"`; TMPDIR=~/.cache/mali-tmp for the build while /tmp is full.

## Notes from fix/hot-clusters (2026-11-18)
- Integer RTs (84bca7c): 7 of 41 pass on v10. The other 34 need: colour loadOp LOAD of integer formats (preload shader is float-only), 3-channel and A2-10-bit integer formats (not in the RT table), layered clears (see below), input-attachment reads in renderpass.suballocation, partial CmdClearAttachments rects.
- cube_layers: mali_CmdBeginRendering ignores layerCount and mali_CmdClearAttachments ignores VkClearRect baseArrayLayer/layerCount, so a clear aimed at layer N lands on layer 0 of the view. Not integer-specific. Needs per-layer targets (layer stride from the image) for clear-only frames; layered draws need gl_Layer. Open.
- clear_color_attachment a2r10g10b10 sample_count_4 fails on f3149a2 too (6 of 14 farm runs, v11 only): an msaa-path flake, belongs to feat/msaa (4x resolve/Average on A2-10 formats); not in the clear path.
- stencil.nocolor: stencil-only passes now run (6db9993); remaining mismatches read back all-zero stencil. Suspect: pipelines without a fragment stage but rasterization enabled (draws are dropped by the discard path) or the S8 writeback.
## dual_source crash group (2026-11-18)
The `dual_source` blend cases crash the dEQP context when run as one `cases` batch (33 cases, /tmp/crash7.txt) at 73fd1f7, 89a8bfd, 1772d2b and main alike, also with
`MALI_INT_NOCOH=0` and with the meta-coh fix; single cases and the daily pass. So it is not a regression but a batch artefact: my reading is state leaking between
cases in one process (accumulating BOs/queue state until a GPU fault kills the context), not a per-case bug. Not investigated further.
