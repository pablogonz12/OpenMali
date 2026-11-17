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
