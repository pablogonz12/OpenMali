# dEQP-VK triage, our ICD vs PanVK-kbase (Poco G925/v13, glibc guest) — 2026-11-17

Status: PARTIAL. Our ICD sample run (`lists/daily-sample.txt`, 1381 cases = every 20th of r3-daily + ≥1 per 4-level group, stress cases removed)
got only through ~78 cases before hanging; the run was cancelled when the phone stalled. Numbers are updated below after each chunk.

## Baselines (PanVK, same list, `runs/qdeqp/poco-panvk-daily-sample.csv`)
1381 cases: 642 pass, 18 fail, 721 NotSupported, 0 crash → 97.3 % of supported (46.5 % of total). All 18 fails are in `api.copy_and_blit.core`
(oracle bugs / format combos; not ours to fix). api.object_management stress group (137 cases): the "137 crashes" were a harness artefact, see below.

## Ours (`runs/qdeqp/poco-ours-daily-sample.tsv`, first ~78 cases, api.* only)
38 Pass, 35 NotSupported, 3 Fail, 1 Other:ResourceError, 1 Crash (asm_triangle) + a hang on `api.command_buffers.submit_twice_secondary`
(process never ends, no QPA growth).

## Failure clusters (ranked by cases seen; sample too small, ranking is provisional)
1. **Any draw / graphics-pipeline case crashes or hangs** (api.smoke.asm_triangle*, command_buffers.submit_twice_secondary; all of pipeline.* and
   renderpass.* are expected to follow). Root cause guess: missing/incorrect draw path (CmdDraw → tiler job, FBD, queue submit with graphics) — a bug/not
   implemented, not a feature flag. Fix location: `driver/vk/` CmdDraw/ render pass / queue submit; get an oracle CS trace to compare. Reproduce
   alone: `qdeqp poco <list with that case> --icd ours`.
2. **`VK_ERROR_OUT_OF_DEVICE_MEMORY` from vkCreateComputePipelines** in `api.descriptor_set.descriptor_set_layout_lifetime.compute` (1 case, ResourceError
   aborts the *whole* deqp process). Root cause guess: pipeline/shader BO allocation not freed or too large per compile (leak in `mali_compile` result
   BO, or pipeline destroy not releasing). Fix location: pipeline create/destroy in driver/vk (kbase mem alloc/free pairing).
3. **api.buffer_view.access.{uniform,storage}_texel_buffer.{r8_unorm,r16g16b16a16_uint}** Fail (2 cases). Root cause guess: texel buffer descriptor
   encoding / format table (tex descriptor for buffers) bug. Fix location: M5/M7 descriptor bytes for buffer views (`vk/tests/build-t_image.sh` golden covers images, probably not buffer views).
4. **api.info.unsupported_image_usage.optimal.input_attachment_bc1_rgba_unorm_block** Fail (1 case; likely a family of block-compressed
   formats). Root cause: ours advertises INPUT_ATTACHMENT usage on a BC format (or image-create doesn't reject it). Fix location: format feature table /
   vkGetPhysicalDeviceImageFormatProperties.
5. NotSupported 35/78 (45 %) — similar to PanVK's 52 % NS on this list, so not a gap in itself; compare per group once more data exists.

## Harness findings (qdeqp.py, fixed)
* The "137 PanVK object_management crashes" were not PanVK: the QPA log was buffered, so when heavy cases (alloc_callback_fail, max_concurrent,
  multithreaded_*; load avg ~25) hit the chunk wall limit, all results were lost, the retry-alone then ran on a starved phone. Fixes: `--deqp-log-flush=enable`;
  a guest-side watchdog (kill on MemAvailable <1.5 GB, load1 >12 after 60 s, or no QPA growth for max(45, case-timeout) s, case recorded as `ResourceHog`,
  session restarted); `calm()` wait (load <8, >2 GB free) before each chunk/retry; stress cases (alloc_callback_fail / max_ / multithreaded / many) are
  excluded by default and written to `lists/stress.txt` (`--stress 1` to run). Verified: 40 of those cases pass on PanVK in one chunk (127 s).
* Ours: a case that dies while running is recorded Crash immediately (no retry); deqp-wide abort on ResourceError is retried alone.
* Hang cost: each hang costs ~3 min until the watchdog fires; run ours with small chunks (≤300) and `--case-timeout 45`.

## Not done
Ours over pipeline.* / renderpass.* (expected 0 pass until draw works); full 1381-case ours run; CMF.
