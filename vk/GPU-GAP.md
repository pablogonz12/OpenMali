# GPU-side gap, ours vs vendor (MaliBridge) on Poco X8 Pro Max (G925 MC11), 2026-11-17

Baseline (BENCH-poco-20261117.md): gpu_ms ours/bridge: cube 0.84/0.17, drawcalls 1.95/1.04, overdraw 2.06/0.84, alu 10.83/5.77, tex 2.24/0.69, compute 1.74/0.85.
Note: "bridge" is the vendor driver; PanVK is the structural oracle (third_party/mesa panvk sources).

## Findings from reading our CS/descriptors vs PanVK (offline)
1. Compute: we never set `COMPUTE_SIZE_WORKGROUP.allow_merging_workgroups` (bit 31; PanVK sets it via `valhall_can_merge_workgroups`) and always used
   task increment 1 on axis X (PanVK: `calculate_task_axis_and_increment`, ~threads_per_task = max_threads/max_tasks_per_core). Both implemented
   (commit "compute allow_merging_workgroups + panvk-style task axis"); env A/B knobs `MALI_NO_MERGE=1`, `MALI_TASK_THREADS=<n>` (default max_threads/4).
2. Every graphics frame and compute job starts with FLUSH_CACHE2(L2 clean+inv, LSC clean+inv, other inv) + WAIT (mali_cs_emit.c `MALI_HF`, default 0x332;
   compute head always (3,3,2)); PanVK does no full cache clean/invalidate between jobs. CMF measurement: MALI_HF=0 MALI_NOTF=1 saved 0.35 ms on cube.
   gpu_ms (timestamps around the CS) includes these.
3. Render targets/depth are linear, no AFBC (CMF: gpu_ms scales with pixels). WSI accepts LINEAR modifier only (mali_wsi_modifier).
4. Fragment path: FBD always Clean Tile Write, colour always stored; tile size/hierarchy: hmask 0 on v13, tile_log2 12 (libkbase default).

## Experiments (Poco, mdbench compute, ours, mailbox, 5-10 s, 1 run each; run-to-run noise +-0.2 ms at x1; temp 44-50 C, hot)
Fixed vs per-unit cost (compute --gpu-scale): ours x0.02 0.67 ms (0.57 with MALI_CHF=0), bridge 0.28; x1 ours 1.36 / bridge 0.87; x8 ours 6.6 / bridge 4.96.
=> ~0.4 ms of the "2x" is FIXED per-job cost inside the timestamp window (head FLUSH_CACHE2(3,3,2)+WAIT ~0.1 ms, tail flush+WAIT, SYNC_ADD64
system scope, ops frames around), not shader time.
Per-unit (x8, 2 runs each): old (NO_MERGE=1 TASK_THREADS=1) 6.41 / 6.57 ms; new (allow_merging_workgroups + task increment) 5.93 / 5.62 ms = -10 %,
per-unit slope 0.58-0.62 ms vs bridge 0.58: per-unit compute gap closed. At x1 the gain is inside the noise (1.35-1.46).
NOT applied: dropping the head flush (MALI_HF=0 / MALI_CHF=0): the head FLUSH_CACHE2+WAIT was the fix for the intermittent tiler-ctx corruption
(libkbase/INTERMITTENT.md); saves ~0.1-0.35 ms. Knobs kept (MALI_HF graphics, MALI_CHF compute) for later validated work.

## Not done / open
- Graphics (cube/overdraw/alu/tex/drawcalls): no change made. Candidates in order: AFBC/tiled RT (needs compositor modifier support, WSI advertises LINEAR only),
  fixed ~0.4 ms per frame (flushes), FBD clean-tile-write/discard. No PanVK job-stream capture was taken (no time, device hot 48-50 C).
- dEQP daily sample could NOT be run: from this session on, the Poco guest aborts deqp-vk at ~case 63-77 with VK_ERROR_OUT_OF_DEVICE_MEMORY in
  api.buffer.suballocation (also with the pre-change source 6cec691, also after qrestore): environment, not this change. (Also: host /tmp is a full tmpfs; qdeqp.py
  now honours QDEQP_TMP.)
- Validation that passed with the new code on the Poco: t_vk_compute (atomic256, atomic_ind, scan...), t_vk_draw, t_vk_meta, t_bc_hw 12/12.

## Experiment 2026-11-17 (Job 2): head FLUSH_CACHE2 A/B on cube (Poco, mailbox, 2 runs x 6-8 s, 43-44 C, `MALI_HF` env, one change at a time)
| MALI_HF (l2,lsc,other) | cube gpu_ms | result |
|---|---|---|
| default 0x332 | 0.85 | baseline |
| 0 (none) | 0.33 | works, but head flush was the INTERMITTENT.md fix: not safe |
| 0x330 (drop "other" inv) | 0.34 | works |
| 0x302 (drop LSC) | 0.35 | works |
| 0x032 (drop L2) | 0.32 | works (risk: stale tiler ctx in L2) |
| 0x112 | 0.32 | works |
| 0x222 / 0x200 (L2 invalidate WITHOUT clean) | - | DEVICE_LOST (QueuePresentKHR -4): L2 clean must stay with inv |
=> the whole cube gap (0.85 vs vendor 0.17) is ~0.5 ms of flush cost; any single component removed recovers it. Candidate to adopt: 0x330 (keeps L2+LSC clean+inv, drops the other-cache/icache inv).
NOT adopted yet: stress validation (t_vk_draw --loop 1000, t_vk_compute, t_vk_meta, t_bc_hw with MALI_HF=330) could not run: the Poco dropped off USB (adb empty) at this point.
Resume: `vk/tests/valrun.sh "MALI_HF=330" 1000` (valguest.sh/valrun.sh push + run draw/compute/meta); if clean, change the default hf in mali_cs_emit.c (graphics line ~883) from 0x332 to 0x330, re-run mdbench cube/drawcalls/overdraw/tex.
No PanVK job-stream capture / AFBC / FBD experiments done.

## Re-check 2026-11-17 (head flush A/B NOT reproduced; 0x330 default NOT adopted)
Same method as the A/B above (bench/run-guest.sh poco ours cube, wayland, mailbox, warmup 60, `ENVV="MALI_HF=<x>"`, a64-glibc-wl ICD, HEAD source + only the default changed), 3 runs x 8 s each, ~44 C:
| MALI_HF | cube gpu_ms |
|---|---|
| 0 | 0.80 |
| 0x330 | 0.88 |
| 0x332 | 0.88 |
| 0 + MALI_NOTF=1 | 0.84 |
Env delivery verified: a temporary stderr log in mali_cs_emit.c printed `hf=0x0 env=0` in the guest, so MALI_HF does reach the driver through run-guest.sh. Stress validation of 0x330 (valrun 1000 loops, t_bc_hw 12/12) was clean, but the 0.33 ms result of the earlier A/B (runs/mdbench/jb2-cube-*) is not reproducible with the current tree (the ICD binary of that A/B was overwritten; fps then 337 vs 295 now). Cause unknown (different binary state or GPU clock/DVFS state). Default left at 0x332; re-test before changing.

## Experiment 2026-11-17 (Job: CMF head flush validation + A/B)
Validation on the CMF (Mali-G615 MC2/v11), `PHONE=cmf vk/tests/valrun.sh "MALI_HF=330" 1000` (valrun.sh now takes PHONE=cmf|poco): t_vk_draw --loop 1000 rc 0 / 0 fails, t_vk_compute rc 0, t_vk_meta rc 0, 89 s. 0x330 is stress-clean on the CMF.
MALI_HF reaches the driver process in the mdbench path: `ENVV="MALI_PROF=1"` printed PROF lines in the guest log (run-guest.sh prefixes ENVV to the reaper command; env is inherited). The identical Poco numbers are therefore not an env-transport problem. Raw: runs/mdbench/cmf-hf/ (mission repo).
cube, CMF, mailbox, wayland, 8 s, warmup 30, one run per guest session (qrestore between), gpu_ms (fps):
| MALI_HF | run1 | run2 | run3 |
|---|---|---|---|
| unset (lib built with the uncommitted 0x330 default) | 1.68 (160) | 2.55 (148) | 2.71 (148) |
| 0 | 1.47 (124) | 1.60 (166) | 1.63 (167) |
| 330 | 2.60 (119) | 1.72 (164) | 2.52 (150) |
| 332 | 1.64 (158) | 2.13 (166) | 1.61 (162) |
=> NO consistent effect of the head flush on the CMF: the numbers are bimodal (~1.6 vs ~2.5 ms) independent of the setting (0x332 has two 1.6 runs, 0x330 two 2.5 runs), i.e. dominated by GPU clock/session state, not by FLUSH_CACHE2. The "0.5 ms" Poco result and the earlier "MALI_HF=0 saves 0.35 ms" CMF note are not reproducible; the Poco worker reached the same conclusion (87e4d6b). Recommendation: do not claim a gain for 0x330.

## 2026-11-17 (perf/gpu-gap, offline review; phones busy/gated, NO new measurement)
Flush suspect: REFUTED by existing data, not by a new run. CMF (v11) HF A/B: no consistent effect, bimodal 1.6/2.5 ms for every MALI_HF value; the Poco 0.33 ms
result did not reproduce (3 runs 0.80-0.88 with MALI_HF=0). CMF cube is 0.87 ms at 320x180 vs 2.13 ms at 720p: ~1.3 ms scales with pixels, only ~0.75 ms is fixed.
Early-Z is on for depth-tested draws (mali_cs_emit.c emit_draw_regs: pixel_kill 0, zs_update 0, fpk allowed), so it is not a late-Z problem.
New untested suspect for the per-pixel part: memory attributes of the RT/depth BOs. (a) every non-exec BO gets BASE_MEM_COHERENT_LOCAL (0x800) on v10/v11/v13
(mali_backend_kbase.c:122, added for v11 SSBO atomics) including colour/depth images; (b) WSI images come from the dma-heap `system-uncached` (mali_backend_kbase.c kbase_bo_alloc,
MALI_BO_EXPORT) unless type 1. Either could slow GPU write-back of a linear 720p RT+depth (~7 MB/frame).
Next A/B (needs phone): same ICD, cube, interleaved, medians: `ENVV="MALI_BO_COHERENT_LOCAL=0"` vs default (cube has no SSBO atomics, so it is safe to test);
then WSI heap system vs system-uncached. If (a) wins: allocate COHERENT_LOCAL only for buffer-capable memory (dedicated image allocs / a separate memory type) instead of all BOs.
Build for the test: `MESA_BUILD=$PWD/../third_party/build-vk-a64-glibc-wl MALI_TARGET=a64-glibc-wl vk/build.sh` (perf/gpu-gap 8619a6f, libvulkan_mali.so sha256 9422ff68436213f0...).

## 2026-11-17 offline dump of the 720p cube frame (fake backend v11, `MALI_FAKE_DUMP`, `libkbase/tools/decode_stream.py show`, FBD decoded by hand against v11.xml)
Command: `MALI_BACKEND=fake MALI_FAKE_GPU=v11 MALI_FAKE_DUMP=f.dump mdbench-native --scene cube --wsi headless --size 1280x720 --frames 2 --icd out/native/libvulkan_mali.so`.
Frame = FLUSH_CACHE2(0x233)+WAIT, RUN_IDVS (cube), RUN_FRAGMENT, FINISH_FRAGMENT, FLUSH_CACHE2(0x11)+WAIT, SYNC_ADD64. What we emit (v11):
- FBD 1280x720, tile = 32x32 (w11 bits 9..12 = 10), colour alloc 4 (4 B x 1024 px), tiler hierarchy mask 0xfe, 1 RT, no pre/post frame DCD (loadOp CLEAR: clear in FBD).
- RT: RGBA8, block format LINEAR (2), row 5120, Clean Tile Write on. ZS ext: D32F, block LINEAR, Clean Tile Write on, Z Write Enable OFF (mdbench storeOp DONT_CARE).
- RSD: early Z (pixel_kill/zs_update 0), FPK allowed. Texture: 1024x1024 RGBA8 4 MiB, linear (MALI_IMAGE_TILED unset), sampled per pixel by the lit cube.
Hypotheses ranked (per-pixel GPU cost; all unproven until the phone session):
1. BO memory attributes: COHERENT_LOCAL on every non-exec BO. CMF A/B (cube, 10 s, mailbox, interleaved): MALI_BO_COHERENT_LOCAL=0 gpu_ms p50 0.79-0.88 vs 1.03-1.15 default, fps 305-338 vs 155-175 (fps gain mostly CPU: mem_sync_bos issues one MEM_SYNC ioctl per BO per submit). A type-2 image-only non-coherent heap (commit 2a2edda) did NOT help (164 fps = default): so the cost is in buffers/arena/other BOs, not RT/depth.
2. Linear 1024^2 texture (no u-interleave): MALI_IMAGE_TILED=1 exists (unverified on hw). Knob ready.
3. ZS Clean Tile Write with Z write off: MALI_ZS_CTW=0 (new, only when depth is not stored). Earlier try: no gain.
4. Linear RT/depth instead of AFBC: needs WSI modifiers; large work; not a knob.
5. Tile size / hierarchy mask: 32x32 + 0xfe as PanVK; no change planned.
6. Frame head/tail FLUSH_CACHE2: refuted (HF A/B).
Session plan (one phone session, interleaved, ENVV): A default | B MALI_BO_COHERENT_LOCAL=0 | E MALI_IMAGE_TILED=1 | F MALI_ZS_CTW=0 | C MALI_WSI_HEAP=cached.

## 2026-11-17 BO-class audit (COHERENT_LOCAL), CMF cube 720p, interleaved, n=1 per variant (not yet 5 pairs)
Classes: internal arena/pool/geo/meta (G, MALI_INT_NOCOH) no gain; non-storage images (D) no gain; non-storage buffers on type 2 (H) no gain (136 vs 131 fps);
skipping per-submit MEM_SYNC for EXEC/EVENT BOs (J, MALI_MEMSYNC_EX=0) 180 vs 141 fps; J+G (K) 186 fps; MALI_BO_COHERENT_LOCAL=0 (B) 284 fps.
=> the per-submit MEM_SYNC loop (CPU) explains part of B; the rest sits in libkbase-internal BOs (heaps, ring) still coherent. Defaults unchanged.

## 2026-11-17/18 culprit isolated: per-submit MEM_SYNC of internal BOs (CPU), not GPU time
Bisect from the fast side on CMF (mdbench cube 720p mailbox, RUNS=3 in one session, medians; ICD 83553d36/d3d5aed3 + env; `MALI_BO_COHERENT_LOCAL=0` plus `MALI_COH_ON=<class>`):
| variant | fps | gpu p50 ms | cpu submit ms | frame p50 ms |
|---|---|---|---|---|
| A default (global coherent) | 170 | 1.15 | 1.19 | 5.18 |
| B =0 (nothing coherent) | 321 | 1.06 | 0.14 | 2.17 |
| ring coherent | 332 | 1.03 | 0.14 | 2.11 |
| hot arena coherent | 313 | 1.05 | 0.15 | 2.19 |
| VkDeviceMemory (lazy) coherent | 329 | 1.03 | 0.14 | 2.10 |
| internal pool/meta/geo coherent (int) | 239 | 1.15 | 0.41 | 3.39 |
GPU time is unchanged; the win is CPU: mem_sync_bos issued one MEM_SYNC ioctl per internal BO per submit (and those BOs are CPU-write-combined, nothing reads them coherently). New v11 default: only VkDeviceMemory keeps COHERENT_LOCAL (SSBO atomics), images/non-storage buffers (type 2) and all internal BOs do not, no per-submit sync. v10/v13 unchanged (NOCOH stripped there). Override: MALI_BO_COHERENT_LOCAL=1 (old) / =0, MALI_COH_ON=ring,hot,int,lazy,wsi.
tools/bench2 on CMF, 5 interleaved pairs, old ICD 193a1e9c (main) vs new bfc42e94 (6cee585), median fps / sub ms:
| variant | old | new |
|---|---|---|
| cube-720p presented | 44.9 / 1.80 | 48.1 / 0.57 |
| cube-720p offscreen | 58.5 / 1.13 | 58.2 / 0.60 |
| triangle-3k presented | 55.3 / 8.12 | 64.5 / 7.23 |
| triangle-3k offscreen | 104.4 / 6.49 | 107.8 / 6.37 |
(the bench2 cube is GPU-bound at 17 ms: unchanged GPU ms.)

## 2026-11-18 frame serialization at present (perf/queue-order)
Cause (code reading): `mali_queue_submit` CPU-waited on every wait semaphore, including the present's wait on the frame's render-done semaphore, so vkQueuePresent blocked until the GPU finished the frame: fif 2/3 behaved like fif 1 (CMF presented 20.3 ms vs 15.1 ms GPU). Fix: skip that wait for semaphores signalled by this queue (in-order ring) ONLY when a fence travels with the buffer (wsi_common's dma-buf sync_file import probe, kernel >= 6.0: CMF 6.1, poco-x8 6.6). poco-x4 (5.10) returns ENOTTY (vk/DEVICE-poco-x4.md), kbase attaches no implicit fence, so there the wait stays and the change is a no-op. Not yet measured on hardware: the compositor (gamescope) honouring the imported fence must be proven on the CMF/x8 with a tearing check before this counts.
poco-x4 cube-720p presented, ours, origin/main 54470e3, mailbox/3img/fif3 (2026-11-18): 85.0 fps, cpu 0.86 ms, GPU 9.15 ms, p50 frame 11.4 ms (bridge p22: 119.6 fps, GPU 7.36 ms). Other fif / tiled A/B rows: none (poco-x4 reached 45 C within one run; bench2 stops at 43 C).
CMF cube-720p presented, ours, perf/memsync-exec b262cdb: 57.7 fps, submit 0.55 ms, GPU 15.13 ms (bridge p25 74.0 fps).

## 2026-11-18 CMF proof for perf/queue-order (cube-720p presented, mailbox/3img, 6-8 s, 3 interleaved pairs, main = 54470e3 vs branch 92168ce, both a64-glibc-wl)
| fif | main fps (3 runs) | branch fps (3 runs) | median |
|---|---|---|---|
| 1 | 42.9 / 40.9 / 42.4 | 40.3 / 45.3 / 41.9 | 42.4 / 41.9 (no change, as expected) |
| 2 | 38.1 / 38.1 / 39.9 | 48.4 / 47.1 / 47.9 | 38.1 / 47.9 |
| 3 | 40.6 / 43.0 / 40.7 | 49.1 / 48.8 / 49.7 | 40.7 / 49.1 |
GPU time p50 14.2-15.8 ms on both (mean ~20 ms: the compositor shares the GPU); the branch moves the wait from vkQueuePresent (present_acquire) to the fence wait. Frame p50 19 -> 16.6 ms at fif2.
Ordering on hardware (`t_queue --only gpufence`, real GPU, 64 MB copy frames): with MALI_PRESENT_FENCE=1 the present returned before the frame finished 40/40 times and the present semaphore's sync_file never signalled before the frame's fence (0 violations, 2 runs); with =0 the present returns after the frame (0/40 early).
gamescope (patches 0123/0124): the commit waits on the client dma-buf fd (epoll POLLIN = all fences incl. the imported sync_file; `commit_fence_ready_now` is a poll(POLLIN)); the 50-screenshot tearing series was NOT run, so the branch stays unmerged.
MALI_IMAGE_TILED=1 cannot be measured: the cube harness fails with EndCommandBuffer = -8 (meta copy/blit/clear reject tiled images, mali_meta_vk.c). An experiment that tiles only the pure attachments (depth) fails the same way in the frame's command buffer. The tiled A/B needs the meta paths first.

## MALI_IMAGE_TILED=1 A/B (CMF, 2026-11-18, branch perf/tiled-images 11252b5)
Meta copy/const tiled addressing + ZS/RT block format; the in-render-pass part is patches/tiled-targets-hot-files.patch. Headless cube checksum tiled == linear (2211a710919157e8). 3 interleaved pairs at fif2: GPU p50 14.7 (linear) vs 14.3 ms (tiled), fps 42.4 vs 42.1: no real gain, so tiled stays opt-in.

## Present-path timeline (CMF, cube-720p, mailbox/3img/fif3, 2026-11-18)
Offscreen (no present) ours 81 fps / GPU 12.3 ms = GPU-bound, no gaps; presented 49 fps. The bridge presents at 74 vs 88 offscreen (+2.1 ms), ours +7.8 ms. Per-frame CSV (ours): acquire+present 5.6 ms, fence wait 7.7 ms, record+submit 0.55 ms, GPU 13.5 ms with every second frame at 17-20 ms (compositor GPU work inside our frame). Bridge: acquire 0.38 ms, fence wait 12.2, GPU 12.6. So the stall is the acquire (image release by the compositor), not our CPU. mdbench --csv now also writes gpu_start_ms/gpu_end_ms for idle-gap analysis; not yet run (CMF went down).
